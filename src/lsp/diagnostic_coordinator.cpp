#include "lsp/diagnostic_coordinator.hpp"
#include "core/uri.hpp"
#include <algorithm>
#include <condition_variable>
#include <unordered_set>

namespace gdscript_lsp {
using json = nlohmann::json;
class DiagnosticCoordinatorImpl {
public:
	DiagnosticCoordinatorImpl(Workspace &workspace, DiagnosticCoordinator::Send output, DiagnosticCoordinator::Serialize serialize) : workspace_(workspace), send_(std::move(output)), serialize_(std::move(serialize)), engine_(std::make_unique<EngineBridge>(
		[this](const std::string &uri, uint64_t generation, json items) { accept_engine(uri, generation, std::move(items)); },
		[this](json state) { engine_state(std::move(state)); })), worker_([this](std::stop_token stop) {
		run(stop);
	}) {}

	~DiagnosticCoordinatorImpl() { stop(); }

	void configure_engine(const EngineConfiguration &config) {
		if (engine_config_ == config) return;
		engine_config_ = config;
		if (engine_started_) { engine_->configure(workspace_.root(), config); schedule_full(); }
	}
	void start_engine() { engine_started_ = true; engine_->configure(workspace_.root(), engine_config_); }
	void reconnect_engine() { engine_->configure(workspace_.root(), engine_config_, true); schedule_full(); }
	json engine_status() const { return engine_->status(); }
	void external_changes(const std::vector<std::string> &uris) {
		for (const auto &uri : uris) {
			auto path = path_for_file_uri(uri);
			if (path && path->filename() == "project.godot") engine_->project_changed();
		}
	}
	void saved(const std::string &uri) {
		engine_->saved(uri);
		schedule_full();
	}
	void pull(const std::string &uri, const json &id) {
		uint64_t generation;
		bool upstream = engine_->ready();
		{
			std::lock_guard lock(mutex_);
			generation = generation_;
			if (auto found = snapshots_.find(uri); found != snapshots_.end() && found->second.generation == generation_ && found->second.engine == engine_->ready()) {
				send_({{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"kind", "full"}, {"items", found->second.items}}}}); return;
			}
			pulls_[id.dump()] = {id, uri};
			if (upstream) dirty_.insert(uri);
			condition_.notify_all();
		}
		// Preserve immediate standalone pulls, including clients that batch shutdown
		// directly after the request. Engine pulls complete asynchronously.
		if (!upstream) publish(uri, generation, false, {}, false);
	}
	void cancel_pull(const json &id) {
		std::lock_guard lock(mutex_);
		if (pulls_.erase(id.dump())) send_({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32800}, {"message", "Request cancelled"}}}});
	}

	uint64_t begin_update() {
		std::lock_guard lock(mutex_);
		++generation_;
		background_.insert(dirty_.begin(), dirty_.end());
		dirty_.clear();
		last_change_ = std::chrono::steady_clock::now();
		condition_.notify_all();
		return generation_;
	}

	void finish_update(uint64_t generation, const std::vector<std::string> &immediate,
			const std::vector<std::string> &affected) {
		std::lock_guard lock(mutex_);
		(void)generation;
		if (stopping_) return;
		for (const auto &uri : immediate) {
			dirty_.insert(uri);
			background_.erase(uri);
		}
		for (const auto &uri : affected) if (!dirty_.contains(uri)) background_.insert(uri);
		last_change_ = std::chrono::steady_clock::now();
		condition_.notify_all();
	}

	void schedule_full() {
		auto stamps = file_stamps();
		auto uris = workspace_.document_uris();
		std::lock_guard lock(mutex_);
		++generation_;
		dirty_.clear();
		background_.clear();
		background_.insert(uris.begin(), uris.end());
		stamps_ = std::move(stamps);
		polling_started_ = true;
		last_change_ = std::chrono::steady_clock::now();
		next_poll_ = poll_interval_.count() > 0 ? last_change_ + poll_interval_ : std::chrono::steady_clock::time_point::max();
		condition_.notify_all();
	}

	void set_open(const std::string &uri, bool open, const std::string &client_uri = {}) {
		std::lock_guard lock(mutex_);
		if (open) {
			open_.insert(uri);
			client_uris_[uri] = client_uri.empty() ? uri : client_uri;
		} else {
			open_.erase(uri);
			client_uris_.erase(uri);
		}
	}

	void set_poll_interval(std::chrono::milliseconds interval) {
		std::lock_guard lock(mutex_);
		poll_interval_ = interval;
		next_poll_ = polling_started_ && interval.count() > 0 ?
			std::chrono::steady_clock::now() + interval : std::chrono::steady_clock::time_point::max();
		condition_.notify_all();
	}

	void request_stop() {
		std::unordered_map<std::string, Pull> pending;
		{
			std::lock_guard lock(mutex_);
			if (stopping_) return;
			stopping_ = true; ++generation_; dirty_.clear(); background_.clear();
			pending.swap(pulls_); worker_.request_stop(); condition_.notify_all();
		}
		engine_->stop();
		for (const auto &[key, pull] : pending) {
			(void)key;
			json items = json::array();
			for (const auto &diagnostic : workspace_.diagnostics(pull.uri)) items.push_back(serialize_(diagnostic));
			send_({{"jsonrpc", "2.0"}, {"id", pull.id}, {"result", {{"kind", "full"}, {"items", std::move(items)}}}});
		}
	}

	void stop() {
		if (!worker_.joinable()) return;
		request_stop();
		worker_.join();
	}

private:
	static constexpr auto full_scan_delay_ = std::chrono::milliseconds(200);
	struct FileStamp {
		std::filesystem::file_time_type modified;
		uintmax_t size = 0;
		auto operator<=>(const FileStamp &) const = default;
	};

	std::unordered_map<std::string, FileStamp> file_stamps() const {
		std::unordered_map<std::string, FileStamp> result;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator iterator(workspace_.root(),
				std::filesystem::directory_options::skip_permission_denied, error), end;
				iterator != end; iterator.increment(error)) {
			if (error) {
				error.clear();
				continue;
			}
			if (iterator->is_directory()) {
				if (iterator->path().filename() == ".git" || std::filesystem::exists(iterator->path() / ".gdignore")) {
					iterator.disable_recursion_pending();
				}
				continue;
			}
			if (iterator->path().extension() != ".gd" && !iterator->path().string().ends_with(".gd.uid") &&
					iterator->path().filename() != "project.godot") continue;
			auto modified = iterator->last_write_time(error);
			if (error) { error.clear(); continue; }
			auto size = iterator->file_size(error);
			if (error) { error.clear(); continue; }
			result[workspace_.uri_for_path(iterator->path())] = {modified, size};
		}
		return result;
	}

	static std::vector<std::string> merged(std::vector<std::string> first, const std::vector<std::string> &second) {
		first.insert(first.end(), second.begin(), second.end());
		std::sort(first.begin(), first.end());
		first.erase(std::unique(first.begin(), first.end()), first.end());
		return first;
	}

	void refresh_external_files() {
		auto current = file_stamps();
		std::unordered_map<std::string, FileStamp> previous;
		std::unordered_set<std::string> open;
		{
			std::lock_guard lock(mutex_);
			previous = stamps_;
			stamps_ = current;
			open = open_;
		}
		std::vector<std::string> changed;
		for (const auto &[uri, stamp] : current) {
			auto found = previous.find(uri);
			if ((found == previous.end() || found->second != stamp) && !open.contains(uri)) changed.push_back(uri);
		}
		for (const auto &[uri, stamp] : previous) {
			(void)stamp;
			if (!current.contains(uri) && !open.contains(uri)) changed.push_back(uri);
		}
		if (changed.empty()) return;
		std::sort(changed.begin(), changed.end());
		changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
		auto generation = begin_update();
		auto affected = workspace_.affected_documents(changed);
		std::string error;
		for (const auto &uri : changed) workspace_.refresh_file(uri, &error);
		external_changes(changed);
		affected = merged(std::move(affected), workspace_.affected_documents(changed));
		finish_update(generation, {}, affected);
	}

	bool publish(const std::string &uri, uint64_t generation, bool force, std::stop_token stop, bool notify = true) {
		{
			std::lock_guard lock(mutex_);
			if (stopping_ || stop.stop_requested()) return false;
			if (generation != generation_) {
				background_.insert(uri);
				return false;
			}
		}
		auto source = workspace_.document_text(uri);
		{
			std::lock_guard lock(mutex_);
			if (generation != generation_ || stopping_) { if (!stopping_) background_.insert(uri); return false; }
			if (source) engine_->submit(uri, source->first, source->second, generation, open_.contains(uri), has_pull(uri));
			else engine_->forget(uri);
		}
		if (source && engine_->ready()) return true;
		json items = json::array();
		for (const auto &diagnostic : workspace_.diagnostics(uri)) items.push_back(serialize_(diagnostic));
		auto version = workspace_.document_version(uri);
		return accept(uri, generation, version, std::move(items), force, stop, !source && engine_->ready(), notify);
	}

	bool has_pull(const std::string &uri) const {
		return std::any_of(pulls_.begin(), pulls_.end(), [&](const auto &entry) { return entry.second.uri == uri; });
	}
	void accept_engine(const std::string &uri, uint64_t generation, json items) {
		accept(uri, generation, workspace_.document_version(uri), std::move(items), false, {}, true);
	}
	void engine_state(json state) {
		schedule_full();
		{
			std::lock_guard lock(mutex_);
			if (stopping_) return;
			send_({{"jsonrpc", "2.0"}, {"method", "gdscript/diagnosticBackendChanged"}, {"params", std::move(state)}});
		}
		std::lock_guard lock(mutex_);
		for (const auto &[key, pull] : pulls_) { (void)key; dirty_.insert(pull.uri); }
		condition_.notify_all();
	}
	bool accept(const std::string &uri, uint64_t generation, int64_t version, json items, bool force, std::stop_token stop, bool engine, bool notify = true) {
		auto cache_key = items.dump();

		bool should_send = false;
		std::string published_uri = uri;
		{
			std::lock_guard lock(mutex_);
			if (stopping_ || stop.stop_requested()) return false;
			if (engine && !engine_->ready()) { background_.insert(uri); condition_.notify_all(); return false; }
			if (!engine && engine_->ready()) { dirty_.insert(uri); condition_.notify_all(); return false; }
			if (generation != generation_) {
				background_.insert(uri);
				return false;
			}
			snapshots_[uri] = {items, generation, engine};
			for (auto it = pulls_.begin(); it != pulls_.end();) {
				if (it->second.uri == uri) {
					send_({{"jsonrpc", "2.0"}, {"id", it->second.id}, {"result", {{"kind", "full"}, {"items", items}}}});
					it = pulls_.erase(it);
				} else ++it;
			}
			if (!notify) return true;
			auto previous = published_.find(uri);
			if (!force && previous != published_.end() && previous->second == cache_key) return true;
			should_send = force || !items.empty() || previous != published_.end();
			if (should_send) published_[uri] = std::move(cache_key);
			if (auto preferred = client_uris_.find(uri); preferred != client_uris_.end()) {
				published_uri = preferred->second;
			}
			if (!should_send) return true;
			json params = {{"uri", published_uri}, {"diagnostics", std::move(items)}};
			if (version >= 0) params["version"] = version;
			send_({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", std::move(params)}});
		}
		return true;
	}

	void run(std::stop_token stop) {
		while (!stop.stop_requested()) {
			std::string uri;
			uint64_t generation = 0;
			bool force = false;
			bool poll = false;
			{
				std::unique_lock lock(mutex_);
				while (!stop.stop_requested() && !stopping_) {
					auto now = std::chrono::steady_clock::now();
					if (!dirty_.empty()) {
						auto found = dirty_.begin();
						uri = *found;
						dirty_.erase(found);
						force = true;
						generation = generation_;
						break;
					}
					if (poll_interval_.count() > 0 && now >= next_poll_) {
						next_poll_ = now + poll_interval_;
						poll = true;
						break;
					}
					if (!background_.empty() && now >= last_change_ + full_scan_delay_) {
						auto found = background_.begin();
						uri = *found;
						background_.erase(found);
						generation = generation_;
						break;
					}
					auto deadline = next_poll_;
					if (!background_.empty()) deadline = std::min(deadline, last_change_ + full_scan_delay_);
					condition_.wait_until(lock, deadline);
				}
				if (stop.stop_requested() || stopping_) return;
			}
			if (poll) refresh_external_files();
			else if (!uri.empty()) publish(uri, generation, force, stop);
		}
	}

	Workspace &workspace_;
	DiagnosticCoordinator::Send send_;
	DiagnosticCoordinator::Serialize serialize_;
	std::mutex mutex_;
	std::condition_variable condition_;
	std::unordered_set<std::string> dirty_;
	std::unordered_set<std::string> background_;
	std::unordered_set<std::string> open_;
	std::unordered_map<std::string, std::string> client_uris_;
	std::unordered_map<std::string, std::string> published_;
	struct Snapshot { json items; uint64_t generation; bool engine; };
	struct Pull { json id; std::string uri; };
	std::unordered_map<std::string, Snapshot> snapshots_;
	std::unordered_map<std::string, Pull> pulls_;
	EngineConfiguration engine_config_;
	bool engine_started_ = false;
	std::unordered_map<std::string, FileStamp> stamps_;
	uint64_t generation_ = 0;
	bool stopping_ = false;
	bool polling_started_ = false;
	std::chrono::steady_clock::time_point last_change_ = std::chrono::steady_clock::now();
	std::chrono::milliseconds poll_interval_{1000};
	std::chrono::steady_clock::time_point next_poll_ = std::chrono::steady_clock::time_point::max();
	std::unique_ptr<EngineBridge> engine_;
	std::jthread worker_;
};

DiagnosticCoordinator::DiagnosticCoordinator(Workspace &workspace, Send send, Serialize serialize) : impl_(std::make_unique<DiagnosticCoordinatorImpl>(workspace, std::move(send), std::move(serialize))) {}
DiagnosticCoordinator::~DiagnosticCoordinator() = default;
uint64_t DiagnosticCoordinator::begin_update() { return impl_->begin_update(); }
void DiagnosticCoordinator::finish_update(uint64_t g, const std::vector<std::string> &i, const std::vector<std::string> &a) { impl_->finish_update(g, i, a); }
void DiagnosticCoordinator::schedule_full() { impl_->schedule_full(); }
void DiagnosticCoordinator::set_open(const std::string &u, bool o, const std::string &c) { impl_->set_open(u, o, c); }
void DiagnosticCoordinator::set_poll_interval(std::chrono::milliseconds i) { impl_->set_poll_interval(i); }
void DiagnosticCoordinator::request_stop() { impl_->request_stop(); }
void DiagnosticCoordinator::configure_engine(const EngineConfiguration &c) { impl_->configure_engine(c); }
void DiagnosticCoordinator::start_engine() { impl_->start_engine(); }
void DiagnosticCoordinator::reconnect_engine() { impl_->reconnect_engine(); }
void DiagnosticCoordinator::external_changes(const std::vector<std::string> &u) { impl_->external_changes(u); }
void DiagnosticCoordinator::saved(const std::string &u) { impl_->saved(u); }
nlohmann::json DiagnosticCoordinator::engine_status() const { return impl_->engine_status(); }
void DiagnosticCoordinator::pull(const std::string &u, const json &i) { impl_->pull(u, i); }
void DiagnosticCoordinator::cancel_pull(const json &i) { impl_->cancel_pull(i); }
} // namespace gdscript_lsp
