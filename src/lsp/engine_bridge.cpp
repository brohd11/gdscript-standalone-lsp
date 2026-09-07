#include "lsp/engine_bridge.hpp"
#include "lsp/engine_transport.hpp"
#include "core/text.hpp"
#include "core/uri.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace gdscript_lsp {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

EngineConfiguration EngineConfiguration::parse(const json &value) {
	if (!value.is_object()) throw std::invalid_argument("diagnostics.engine must be an object");
	EngineConfiguration config;
	config.mode = value.value("mode", "off");
	if (config.mode != "off" && config.mode != "attach" && config.mode != "launch") throw std::invalid_argument("engine.mode must be off, attach or launch");
	config.executable = value.value("executable", "");
	if (config.mode == "launch" && config.executable.empty()) throw std::invalid_argument("engine.executable is required in launch mode");
	if (config.mode == "attach") {
		if (!value.contains("port") || !value["port"].is_number_integer()) throw std::invalid_argument("engine.port is required in attach mode");
		auto port = value["port"].get<int64_t>();
		if (port < 1 || port > 65535) throw std::invalid_argument("engine.port must be between 1 and 65535");
		config.port = static_cast<uint16_t>(port);
	}
	return config;
}

EngineBridge::EngineBridge(Result result, State state) : result_(std::move(result)), state_(std::move(state)), worker_([this](std::stop_token stop) { run(stop); }) {}
EngineBridge::~EngineBridge() { stop(); }
void EngineBridge::stop() {
	{ std::lock_guard lock(mutex_); stopping_ = true; ++epoch_; worker_.request_stop(); condition_.notify_all(); }
	if (worker_.joinable()) worker_.join();
}
void EngineBridge::configure(const std::filesystem::path &root, const EngineConfiguration &config, bool force) {
	std::lock_guard lock(mutex_);
	if (!force && root == root_ && config == config_ && !reload_required_) return;
	root_ = root; config_ = config; reload_required_ = false; ++epoch_;
	// Invalidate authority immediately, before the worker finishes its old I/O.
	status_["mode"] = config.mode; status_["state"] = config.mode == "off" ? "off" : "connecting";
	status_["reason"] = ""; status_["engineVersion"] = nullptr;
	if (config.mode == "off") { documents_.clear(); pending_saves_.clear(); }
	for (auto &[uri, document] : documents_) { (void)uri; document.queued = true; }
	condition_.notify_all();
}
void EngineBridge::submit(const std::string &uri, std::string source, int64_t version, uint64_t revision, bool open, bool immediate) {
	std::lock_guard lock(mutex_);
	if (config_.mode == "off" || stopping_) return;
	auto found = documents_.find(uri);
	if (found != documents_.end() && found->second.revision == revision && found->second.source == source && found->second.open == open) return;
	bool saved = pending_saves_.contains(uri) || (found != documents_.end() && found->second.saved);
	documents_[uri] = {std::move(source), version, revision, open, true, saved, immediate || open, Clock::now() + (immediate ? 0ms : 150ms)};
	condition_.notify_all();
}
void EngineBridge::saved(const std::string &uri) {
	std::lock_guard lock(mutex_);
	if (config_.mode == "off") return;
	pending_saves_.insert(uri);
	if (auto found = documents_.find(uri); found != documents_.end()) {
		found->second.saved = true; found->second.queued = true; found->second.due = Clock::now();
	}
	condition_.notify_all();
}
void EngineBridge::forget(const std::string &uri) { std::lock_guard lock(mutex_); documents_.erase(uri); pending_saves_.erase(uri); condition_.notify_all(); }
void EngineBridge::project_changed() {
	std::lock_guard lock(mutex_);
	if (config_.mode == "off") return;
	++epoch_; reload_required_ = config_.mode == "attach";
	status_["state"] = reload_required_ ? "reload-required" : "connecting";
	condition_.notify_all();
}
json EngineBridge::status() const { std::lock_guard lock(mutex_); return status_; }
bool EngineBridge::ready() const { std::lock_guard lock(mutex_); return status_["state"] == "ready"; }
void EngineBridge::set_state(uint64_t epoch, std::string state, std::string reason, std::string version) {
	json value;
	{
		std::lock_guard lock(mutex_);
		if (epoch != epoch_ || stopping_) return;
		value = {{"mode", config_.mode}, {"state", std::move(state)}, {"reason", std::move(reason)}, {"engineVersion", version.empty() ? json(nullptr) : json(version)}};
		status_ = value;
	}
	state_(std::move(value));
}

namespace {
json notification(std::string method, json params) { return {{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}}; }
void validate_diagnostics(const json &items) {
	if (!items.is_array()) throw std::runtime_error("Invalid engine diagnostics array");
	for (const auto &item : items) {
		if (!item.is_object() || !item.contains("message") || !item["message"].is_string() || !item.contains("range")) throw std::runtime_error("Malformed engine diagnostic");
		for (auto end : {"start", "end"}) for (auto coordinate : {"line", "character"}) {
			const auto &value = item.at("range").at(end).at(coordinate);
			if (!value.is_number_integer() || value.get<int64_t>() < 0) throw std::runtime_error("Malformed engine diagnostic range");
		}
	}
}
}

void EngineBridge::run(std::stop_token stop) {
	unsigned failures = 0;
	uint64_t previous_epoch = 0;
	while (!stop.stop_requested()) {
		EngineConfiguration config;
		std::filesystem::path root;
		uint64_t epoch;
		bool reload;
		{
			std::unique_lock lock(mutex_);
			condition_.wait(lock, [&] { return stopping_ || (epoch_ != previous_epoch) || (!root_.empty() && config_.mode != "off" && !reload_required_); });
			if (stopping_) return;
			epoch = epoch_; config = config_; root = root_; reload = reload_required_;
			if (epoch != previous_epoch) failures = 0;
			previous_epoch = epoch;
		}
		if (config.mode == "off" || reload || root.empty()) {
			set_state(epoch, reload ? "reload-required" : "off", reload ? "Reload the project in Godot, then request gdscript/reconnectDiagnosticEngine." : "");
			continue;
		}
		EngineProcess process;
		std::string version;
		auto cancelled = [&] {
			if (config.mode == "launch") process.log();
			std::lock_guard lock(mutex_); return stopping_ || epoch != epoch_;
		};
		try {
			set_state(epoch, "connecting");
			uint16_t port = config.port;
			auto deadline = Clock::now() + (config.mode == "launch" ? 60s : 5s);
			if (config.mode == "launch") {
				auto probe = [&](const std::string &argument) {
					process.start(config.executable, {argument});
					auto until = Clock::now() + 5s;
					while (process.running()) {
						if (cancelled() || Clock::now() >= until) throw std::runtime_error("Godot executable probe timed out or was cancelled");
						std::this_thread::sleep_for(20ms);
					}
					return trim(process.log());
				};
				version = probe("--version");
				if (version.empty() || probe("--help").find("--lsp-port") == std::string::npos) throw std::runtime_error("Executable must be a Godot editor build with LSP support");
				auto ports = engine_unused_ports(3); port = ports[0];
				process.start(config.executable, {"--headless", "--editor", "--path", root.string(), "--lsp-port", std::to_string(port),
					"--dap-port", std::to_string(ports[1]), "--debug-server", "tcp://127.0.0.1:" + std::to_string(ports[2])});
			}
			EngineConnection connection;
			auto connected_cancelled = [&] {
				if (cancelled()) return true;
				if (config.mode == "launch" && !process.running()) throw std::runtime_error("Godot exited: " + process.log());
				return false;
			};
			connection.connect(port, deadline, connected_cancelled);
			uint64_t request_id = 1;
			auto send = [&](const std::string &method, json params, bool request = false) {
				auto message = notification(method, std::move(params));
				if (request) message["id"] = ++request_id;
				connection.send(message, deadline, connected_cancelled);
			};
			auto handle_message = [&](const json &message) {
				auto method = message.value("method", "");
				if (method == "gdscript_client/changeWorkspace") {
					auto supplied = message.at("params").value("path", "");
					std::error_code error;
					if (!std::filesystem::equivalent(root, supplied, error) || error) throw std::runtime_error("Godot has a different project open");
				}
				if (!method.empty() && message.contains("id")) {
					connection.send({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"error", {{"code", -32601}, {"message", "Diagnostic bridge does not support client actions"}}}}, deadline, connected_cancelled);
				}
			};
			send("initialize", {{"rootUri", file_uri_for_path(root)}, {"rootPath", root.string()}, {"capabilities", json::object()}}, true);
			while (true) {
				auto message = connection.receive(deadline, connected_cancelled); handle_message(message);
				if (message.contains("id") && message["id"] == request_id) {
					const auto &capabilities = message.at("result").at("capabilities");
					const auto &sync = capabilities.at("textDocumentSync");
					int kind = sync.is_number_integer() ? sync.get<int>() : sync.value("change", 0);
					if (kind != 1 || !capabilities.value("documentSymbolProvider", false)) throw std::runtime_error("Engine lacks required full-text synchronization or document symbols");
					break;
				}
			}
			send("initialized", json::object());
			while (true) {
				auto message = connection.receive(deadline, connected_cancelled); handle_message(message);
				if (message.value("method", "") == "gdscript/capabilities") break;
			}
			{
				std::lock_guard lock(mutex_);
				for (auto &[uri, document] : documents_) { (void)uri; document.queued = true; }
			}
			set_state(epoch, "ready", {}, version);
			std::unordered_set<std::string> opened;
			while (!connected_cancelled()) {
				deadline = Clock::now() + 5s;
				Document document;
				std::string uri;
				std::vector<std::string> close;
				{
					std::lock_guard lock(mutex_);
					for (const auto &path : opened) if (!documents_.contains(path) || !documents_.at(path).open) close.push_back(path);
					for (auto &[path, candidate] : documents_) {
						if (!candidate.queued || candidate.due > Clock::now()) continue;
						if (uri.empty() || (candidate.priority && !document.priority) || (candidate.priority == document.priority && candidate.due < document.due)) { uri = path; document = candidate; }
					}
					if (!uri.empty()) { documents_.at(uri).queued = false; documents_.at(uri).saved = false; pending_saves_.erase(uri); }
				}
				for (const auto &path : close) { send("textDocument/didClose", {{"textDocument", {{"uri", path}}}}); opened.erase(path); }
				if (uri.empty()) {
					if (connection.readable()) handle_message(connection.receive(deadline, connected_cancelled));
					std::unique_lock lock(mutex_); condition_.wait_for(lock, 25ms); continue;
				}
				if (opened.contains(uri)) send("textDocument/didChange", {{"textDocument", {{"uri", uri}, {"version", document.version}}}, {"contentChanges", json::array({{{"text", document.source}}})}});
				else {
					send("textDocument/didOpen", {{"textDocument", {{"uri", uri}, {"languageId", "gdscript"}, {"version", document.version}, {"text", document.source}}}});
					opened.insert(uri);
				}
				if (document.saved) send("textDocument/didSave", {{"textDocument", {{"uri", uri}}}, {"text", document.source}});
				send("textDocument/documentSymbol", {{"textDocument", {{"uri", uri}}}}, true);
				std::optional<json> diagnostics;
				while (true) {
					auto message = connection.receive(deadline, connected_cancelled); handle_message(message);
					if (message.value("method", "") == "textDocument/publishDiagnostics") {
						const auto &params = message.at("params");
						if (canonical_file_uri(params.value("uri", "")) == canonical_file_uri(uri)) {
							validate_diagnostics(params.at("diagnostics")); diagnostics = params["diagnostics"];
						}
					}
					if (message.contains("id") && message["id"] == request_id) {
						if (message.contains("error") || !diagnostics) throw std::runtime_error("Engine did not complete diagnostic synchronization");
						break;
					}
				}
				if (!document.open) { send("textDocument/didClose", {{"textDocument", {{"uri", uri}}}}); opened.erase(uri); }
				bool current;
				{
					std::lock_guard lock(mutex_);
					auto found = documents_.find(uri);
					current = epoch == epoch_ && found != documents_.end() && found->second.revision == document.revision;
				}
				if (current) { failures = 0; result_(uri, document.revision, std::move(*diagnostics)); }
			}
		} catch (const std::exception &error) {
			if (!cancelled()) set_state(epoch, "fallback", error.what(), version);
		}
		process.stop();
		std::unique_lock lock(mutex_);
		if (epoch == epoch_ && !stopping_) {
			auto delay = std::chrono::seconds(std::min(30u, 1u << std::min(failures++, 5u)));
			condition_.wait_for(lock, delay, [&] { return stopping_ || epoch != epoch_; });
		}
	}
}
} // namespace gdscript_lsp
