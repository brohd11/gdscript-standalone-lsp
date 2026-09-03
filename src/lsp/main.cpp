#include "core/text.hpp"
#include "core/uri.hpp"
#include "core/workspace.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;
using namespace gdscript_lsp;

namespace {

json position_json(Position value) {
	return {{"line", value.line}, {"character", value.character}};
}

json range_json(Range value) {
	return {{"start", position_json(value.start)}, {"end", position_json(value.end)}};
}

Position parse_position(const json &value) {
	return {value.value("line", 0U), value.value("character", 0U)};
}

Range parse_range(const json &value) {
	return {parse_position(value.value("start", json::object())), parse_position(value.value("end", json::object()))};
}

json symbol_json(const Symbol &symbol) {
	json children = json::array();
	for (const auto &child : symbol.children) children.push_back(symbol_json(child));
	json result = {
		{"name", symbol.name},
		{"detail", symbol.detail},
		{"kind", static_cast<int>(symbol.kind)},
		{"range", range_json(symbol.range)},
		{"selectionRange", range_json(symbol.selection_range)}
	};
	if (!children.empty()) result["children"] = std::move(children);
	return result;
}

json completion_json(const CompletionItem &item) {
	json result = {
		{"label", item.label},
		{"kind", static_cast<int>(item.kind)},
		{"detail", item.detail},
		{"insertText", item.insert_text.empty() ? item.label : item.insert_text},
		{"filterText", item.filter_text.empty() ? item.label : item.filter_text},
		{"sortText", item.sort_text}
	};
	if (!item.documentation.empty()) result["documentation"] = {{"kind", "markdown"}, {"value", item.documentation}};
	if (!item.symbol_id.empty() || !item.origin_id.empty() || !item.provider.empty() || !item.access_kind.empty()) {
		result["data"] = {{"gdscriptLsp", {
			{"symbolId", item.symbol_id}, {"originId", item.origin_id},
			{"provider", item.provider}, {"accessKind", item.access_kind}
		}}};
	}
	return result;
}

json type_json(const ResolvedType &type) {
	json arguments = json::array();
	for (const auto &argument : type.arguments) arguments.push_back(type_json(argument));
	return {
		{"kind", type_kind_name(type.kind)},
		{"name", type.name},
		{"display", type.display()},
		{"symbolId", type.symbol_id},
		{"instance", type.instance},
		{"arguments", std::move(arguments)}
	};
}

json origin_json(const SymbolOrigin &origin) {
	return {
		{"symbolId", origin.symbol_id}, {"uri", origin.uri}, {"ownerId", origin.owner_id},
		{"name", origin.name}, {"kind", static_cast<int>(origin.kind)}, {"range", range_json(origin.range)}
	};
}

json expression_json(const ResolvedExpression &expression) {
	json paths = json::array();
	for (const auto &path : expression.access_paths) paths.push_back({
		{"text", path.text}, {"kind", access_path_kind_name(path.kind)}, {"preferred", path.preferred}
	});
	json result = {{"type", type_json(expression.type)}, {"origin", nullptr}, {"accessPaths", std::move(paths)}};
	if (expression.origin) result["origin"] = origin_json(*expression.origin);
	return result;
}

json diagnostic_json(const Diagnostic &diagnostic) {
	json related = json::array();
	for (const auto &item : diagnostic.related_information) {
		related.push_back({{"location", {{"uri", item.location.uri}, {"range", range_json(item.location.range)}}},
			{"message", item.message}});
	}
	json result = {
		{"range", range_json(diagnostic.range)},
		{"severity", static_cast<int>(diagnostic.severity)},
		{"code", diagnostic.code},
		{"source", diagnostic.source},
		{"message", diagnostic.message},
	};
	if (!related.empty()) result["relatedInformation"] = std::move(related);
	return result;
}

std::optional<json> read_message() {
	std::string line;
	size_t length = 0;
	while (std::getline(std::cin, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) break;
		constexpr std::string_view header = "Content-Length:";
		if (line.starts_with(header)) length = static_cast<size_t>(std::stoul(line.substr(header.size())));
	}
	if (!std::cin || length == 0) return std::nullopt;
	std::string payload(length, '\0');
	std::cin.read(payload.data(), static_cast<std::streamsize>(length));
	try {
		return json::parse(payload);
	} catch (const std::exception &exception) {
		std::cerr << "gdscript-lsp: invalid JSON: " << exception.what() << '\n';
		return json::object();
	}
}

void send(const json &message) {
	static std::mutex output_mutex;
	std::lock_guard lock(output_mutex);
	auto body = message.dump();
	std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}

class DiagnosticPublisher {
public:
	explicit DiagnosticPublisher(Workspace &workspace) : workspace_(workspace), worker_([this](std::stop_token stop) {
		run(stop);
	}) {}

	~DiagnosticPublisher() { stop(); }

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

	void set_open(const std::string &uri, bool open) {
		std::lock_guard lock(mutex_);
		if (open) open_.insert(uri);
		else open_.erase(uri);
	}

	void set_poll_interval(std::chrono::milliseconds interval) {
		std::lock_guard lock(mutex_);
		poll_interval_ = interval;
		next_poll_ = polling_started_ && interval.count() > 0 ?
			std::chrono::steady_clock::now() + interval : std::chrono::steady_clock::time_point::max();
		condition_.notify_all();
	}

	void request_stop() {
		std::lock_guard lock(mutex_);
		if (stopping_) return;
		stopping_ = true;
		++generation_;
		dirty_.clear();
		background_.clear();
		worker_.request_stop();
		condition_.notify_all();
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
		affected = merged(std::move(affected), workspace_.affected_documents(changed));
		finish_update(generation, {}, affected);
	}

	bool publish(const std::string &uri, uint64_t generation, bool force, std::stop_token stop) {
		{
			std::lock_guard lock(mutex_);
			if (stopping_ || stop.stop_requested()) return false;
			if (generation != generation_) {
				background_.insert(uri);
				return false;
			}
		}
		json items = json::array();
		for (const auto &diagnostic : workspace_.diagnostics(uri)) items.push_back(diagnostic_json(diagnostic));
		auto version = workspace_.document_version(uri);
		auto cache_key = items.dump();

		bool should_send = false;
		{
			std::lock_guard lock(mutex_);
			if (stopping_ || stop.stop_requested()) return false;
			if (generation != generation_) {
				background_.insert(uri);
				return false;
			}
			auto previous = published_.find(uri);
			if (previous != published_.end() && previous->second == cache_key) return true;
			should_send = force || !items.empty() || previous != published_.end();
			if (should_send) published_[uri] = std::move(cache_key);
		}
		if (!should_send) return true;
		json params = {{"uri", uri}, {"diagnostics", std::move(items)}};
		if (version >= 0) params["version"] = version;
		send({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", std::move(params)}});
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
	std::mutex mutex_;
	std::condition_variable condition_;
	std::unordered_set<std::string> dirty_;
	std::unordered_set<std::string> background_;
	std::unordered_set<std::string> open_;
	std::unordered_map<std::string, std::string> published_;
	std::unordered_map<std::string, FileStamp> stamps_;
	uint64_t generation_ = 0;
	bool stopping_ = false;
	bool polling_started_ = false;
	std::chrono::steady_clock::time_point last_change_ = std::chrono::steady_clock::now();
	std::chrono::milliseconds poll_interval_{1000};
	std::chrono::steady_clock::time_point next_poll_ = std::chrono::steady_clock::time_point::max();
	std::jthread worker_;
};

void respond(const json &id, json result) {
	send({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void respond_error(const json &id, int code, const std::string &message) {
	send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

void apply_content_changes(std::string &text, const json &changes) {
	for (const auto &change : changes) {
		if (!change.contains("range")) {
			text = change.value("text", "");
			continue;
		}
		auto range = parse_range(change["range"]);
		auto begin = position_to_byte(text, range.start);
		auto end = position_to_byte(text, range.end);
		if (begin > end || end > text.size()) continue;
		text.replace(begin, end - begin, change.value("text", ""));
	}
}

std::optional<std::filesystem::path> find_project_root(std::filesystem::path candidate) {
	if (candidate.empty()) return std::nullopt;
	std::error_code error;
	candidate = std::filesystem::absolute(candidate, error).lexically_normal();
	if (error) return std::nullopt;
	if (std::filesystem::is_regular_file(candidate, error)) candidate = candidate.parent_path();
	while (!candidate.empty()) {
		if (std::filesystem::is_regular_file(candidate / "project.godot", error)) {
			auto canonical = std::filesystem::weakly_canonical(candidate, error);
			return error ? std::optional<std::filesystem::path>(candidate) : std::optional<std::filesystem::path>(canonical);
		}
		auto parent = candidate.parent_path();
		if (parent == candidate) break;
		candidate = std::move(parent);
	}
	return std::nullopt;
}

void add_project_root(std::vector<std::filesystem::path> &roots, const std::filesystem::path &candidate) {
	auto root = find_project_root(candidate);
	if (!root) return;
	if (std::find(roots.begin(), roots.end(), *root) == roots.end()) roots.push_back(std::move(*root));
}

void add_project_uri(std::vector<std::filesystem::path> &roots, const std::string &uri) {
	if (auto path = path_for_file_uri(uri)) add_project_root(roots, *path);
}

std::optional<std::filesystem::path> project_from_initialize(const json &params, std::string &error) {
	std::vector<std::filesystem::path> roots;
	auto folders = params.find("workspaceFolders");
	if (folders != params.end() && folders->is_array()) {
		for (const auto &folder : *folders) {
			if (folder.is_object()) add_project_uri(roots, folder.value("uri", ""));
		}
	}
	if (roots.size() > 1) {
		error = "multiple Godot project roots are not supported by one server process";
		return std::nullopt;
	}
	if (roots.size() == 1) return roots.front();

	if (auto root_uri = params.find("rootUri"); root_uri != params.end() && root_uri->is_string()) {
		add_project_uri(roots, root_uri->get<std::string>());
	}
	if (roots.empty()) {
		if (auto root_path = params.find("rootPath"); root_path != params.end() && root_path->is_string()) {
			add_project_root(roots, root_path->get<std::string>());
		}
	}
	if (roots.empty()) {
		std::error_code current_error;
		auto current = std::filesystem::current_path(current_error);
		if (!current_error) add_project_root(roots, current);
	}
	if (roots.empty()) {
		error = "no Godot project found; open a folder containing project.godot or pass --project";
		return std::nullopt;
	}
	return roots.front();
}

std::filesystem::path discover_api(const std::filesystem::path &project, const std::filesystem::path &configured,
	const char *executable_path) {
	if (!configured.empty()) return configured;
	if (const char *environment_api = std::getenv("GDSCRIPT_LSP_API"); environment_api && *environment_api) {
		return environment_api;
	}
	for (const auto &candidate : {
		project / ".godot/addons/gdscript_parser/extension_api.json",
		project / "addons/gdscript_lsp/data/godot-4.6-extension-api.json"}) {
		if (std::filesystem::exists(candidate)) return candidate;
	}
	std::error_code error;
	std::filesystem::path executable = executable_path;
	if (!executable.has_parent_path()) {
		if (const char *path_value = std::getenv("PATH")) {
#ifdef _WIN32
			constexpr char separator = ';';
#else
			constexpr char separator = ':';
#endif
			std::string_view paths = path_value;
			while (!paths.empty()) {
				auto end = paths.find(separator);
				auto directory = paths.substr(0, end);
				auto candidate = std::filesystem::path(directory.empty() ? "." : directory) / executable;
				if (std::filesystem::is_regular_file(candidate, error)) {
					executable = std::move(candidate);
					break;
				}
				if (end == std::string_view::npos) break;
				paths.remove_prefix(end + 1);
			}
		}
	}
	executable = std::filesystem::weakly_canonical(executable, error);
	if (error) return {};
	for (const auto &candidate : {
		executable.parent_path().parent_path() / "addons/gdscript_lsp/data/godot-4.6-extension-api.json",
		executable.parent_path().parent_path() / "share/gdscript-lsp/godot-4.6-extension-api.json"}) {
		if (std::filesystem::exists(candidate)) return candidate;
	}
	return {};
}

json initialize_result() {
	return {
		{"capabilities", {
			{"positionEncoding", "utf-16"},
			{"textDocumentSync", {{"openClose", true}, {"change", 2}, {"save", {{"includeText", false}}}}},
			{"completionProvider", {{"triggerCharacters", json::array({".", "(", ",", ":", "=", "\"", "'"})},
				{"resolveProvider", true}}},
			{"hoverProvider", true},
			{"definitionProvider", true},
			{"documentSymbolProvider", true},
			{"diagnosticProvider", {{"identifier", "gdscript-lsp"}, {"interFileDependencies", true},
				{"workspaceDiagnostics", false}}}
		}},
		{"serverInfo", {{"name", "gdscript-lsp"}, {"version", "0.1.0"}}}
	};
}

std::vector<std::string> merge_uris(std::vector<std::string> first, const std::vector<std::string> &second) {
	first.insert(first.end(), second.begin(), second.end());
	std::sort(first.begin(), first.end());
	first.erase(std::unique(first.begin(), first.end()), first.end());
	return first;
}

void apply_configuration(Workspace &workspace, DiagnosticPublisher &publisher, const json &settings) {
	if (!settings.is_object()) return;
	const json *root = &settings;
	if (auto found = root->find("gdscriptLsp"); found != root->end() && found->is_object()) root = &*found;
	auto completion = root->find("completion");
	if (completion != root->end() && completion->is_object()) {
		auto config = workspace.completion_config();
		auto boolean = [&](const char *name, bool &target) {
			if (auto found = completion->find(name); found != completion->end() && found->is_boolean()) target = found->get<bool>();
		};
		boolean("enums", config.enums);
		boolean("extendedTypeHints", config.extended_type_hints);
		boolean("constructors", config.constructors);
		boolean("hidePrivate", config.hide_private);
		if (auto member_strings = completion->find("memberStrings"); member_strings != completion->end()) {
			if (member_strings->is_boolean()) config.member_strings = member_strings->get<bool>();
			else if (member_strings->is_object()) {
				auto member_boolean = [&](const char *name, bool &target) {
					if (auto found = member_strings->find(name); found != member_strings->end() && found->is_boolean()) {
						target = found->get<bool>();
					}
				};
				member_boolean("enabled", config.member_strings);
				member_boolean("preferStringName", config.member_strings_prefer_string_name);
				member_boolean("includePrivate", config.member_strings_include_private);
			}
		}
		workspace.set_completion_config(config);
	}
	if (auto diagnostics = root->find("diagnostics"); diagnostics != root->end() && diagnostics->is_object()) {
		if (auto interval = diagnostics->find("pollIntervalMs"); interval != diagnostics->end() && interval->is_number_integer()) {
			auto milliseconds = interval->get<int64_t>();
			if (milliseconds > 0) milliseconds = std::max<int64_t>(milliseconds, 100);
			publisher.set_poll_interval(std::chrono::milliseconds(std::max<int64_t>(milliseconds, 0)));
		}
	}
}

} // namespace

int main(int argc, char **argv) {
	std::filesystem::path project;
	std::filesystem::path configured_api;
	for (int index = 1; index < argc; ++index) {
		std::string argument = argv[index];
		if (argument == "--project" && index + 1 < argc) project = argv[++index];
		else if (argument == "--api" && index + 1 < argc) configured_api = argv[++index];
		else if (argument == "--version") {
			std::cout << "gdscript-lsp 0.1.0 (Godot 4.6)\n";
			return 0;
		}
	}
	Workspace workspace;
	DiagnosticPublisher diagnostics(workspace);
	std::string error;
	std::unordered_map<std::string, std::string> buffers;
	std::unordered_set<std::string> cancelled;
	bool initialized = false;
	bool shutdown = false;
	while (auto message = read_message()) {
		if (!message->is_object() || !message->contains("method")) continue;
		auto method = message->value("method", "");
		auto params = message->value("params", json::object());
		bool request = message->contains("id");
		json id = request ? (*message)["id"] : json();

		if (method == "$/cancelRequest") {
			cancelled.insert(params.contains("id") ? params["id"].dump() : "null");
			continue;
		}
		if (request && cancelled.erase(id.dump()) != 0) {
			respond_error(id, -32800, "Request cancelled");
			continue;
		}
		if (method == "initialize") {
			error.clear();
			if (initialized) {
				respond_error(id, -32600, "initialize may only be sent once");
				continue;
			}
			if (auto options = params.find("initializationOptions"); options != params.end()) {
				apply_configuration(workspace, diagnostics, *options);
			}
			auto selected_project = project.empty() ? project_from_initialize(params, error) : find_project_root(project);
			if (!selected_project) {
				if (error.empty()) error = "project.godot not found at or above " + project.string();
				respond_error(id, -32602, error);
				continue;
			}
			auto api = discover_api(*selected_project, configured_api, argv[0]);
			if (!workspace.open(*selected_project, api, &error)) {
				respond_error(id, -32603, "could not index Godot project: " + error);
				continue;
			}
			initialized = true;
			const auto &stats = workspace.stats();
			std::cerr << "gdscript-lsp: indexed " << stats.document_count << " documents / " << stats.class_count
					  << " classes in " << stats.elapsed_ms << " ms; syntax errors: " << stats.syntax_error_count << '\n';
			respond(id, initialize_result());
		} else if (method == "exit") {
			return shutdown ? 0 : 1;
		} else if (!initialized) {
			if (request) respond_error(id, -32002, "Server not initialized");
		} else if (method == "initialized") {
			diagnostics.schedule_full();
		} else if (method == "shutdown") {
			shutdown = true;
			diagnostics.request_stop();
			respond(id, nullptr);
		} else if (method == "textDocument/didOpen") {
			auto document = params["textDocument"];
			auto uri = document.value("uri", "");
			auto text = document.value("text", "");
			buffers[uri] = text;
			diagnostics.set_open(uri, true);
			auto generation = diagnostics.begin_update();
			UpdateImpact impact;
			if (workspace.update_document(uri, std::move(text), document.value("version", -1), &error, &impact)) {
				diagnostics.finish_update(generation, {uri}, impact.affected_documents);
			} else diagnostics.set_open(uri, false);
		} else if (method == "textDocument/didChange") {
			auto document = params["textDocument"];
			auto uri = document.value("uri", "");
			if (!buffers.contains(uri)) buffers[uri] = "";
			apply_content_changes(buffers[uri], params.value("contentChanges", json::array()));
			auto generation = diagnostics.begin_update();
			UpdateImpact impact;
			if (workspace.update_document(uri, buffers[uri], document.value("version", -1), &error, &impact)) {
				diagnostics.finish_update(generation, {uri}, impact.affected_documents);
			}
		} else if (method == "textDocument/didClose") {
			auto uri = params["textDocument"].value("uri", "");
			buffers.erase(uri);
			diagnostics.set_open(uri, false);
			auto generation = diagnostics.begin_update();
			auto affected = workspace.affected_documents({uri});
			if (workspace.close_document(uri, &error)) {
				affected = merge_uris(std::move(affected), workspace.affected_documents({uri}));
				diagnostics.finish_update(generation, {uri}, affected);
			}
		} else if (method == "workspace/didChangeConfiguration") {
			apply_configuration(workspace, diagnostics, params.value("settings", json::object()));
		} else if (method == "workspace/didChangeWatchedFiles") {
			auto generation = diagnostics.begin_update();
			std::vector<std::string> changed;
			for (const auto &change : params.value("changes", json::array())) {
				auto uri = change.value("uri", "");
				if (!buffers.contains(uri)) changed.push_back(uri);
			}
			auto affected = workspace.affected_documents(changed);
			for (const auto &uri : changed) workspace.refresh_file(uri, &error);
			affected = merge_uris(std::move(affected), workspace.affected_documents(changed));
			diagnostics.finish_update(generation, {}, affected);
		} else if (method == "textDocument/completion") {
			auto uri = params["textDocument"].value("uri", "");
			auto completion = workspace.completion_result(uri, parse_position(params["position"]));
			json output = json::array();
			for (const auto &item : completion.items) output.push_back(completion_json(item));
			respond(id, {{"isIncomplete", completion.is_incomplete}, {"items", std::move(output)}});
		} else if (method == "completionItem/resolve") {
			auto item = params;
			std::string symbol_id;
			if (auto data = item.find("data"); data != item.end() && data->is_object()) {
				if (auto extension = data->find("gdscriptLsp"); extension != data->end() && extension->is_object()) {
					symbol_id = extension->value("symbolId", "");
				}
			}
			if (auto resolved = workspace.resolve_completion_item(symbol_id)) {
				auto enriched = completion_json(*resolved);
				for (auto key : {"detail", "documentation"}) {
					if (enriched.contains(key)) item[key] = enriched[key];
				}
			}
			respond(id, std::move(item));
		} else if (method == "textDocument/hover") {
			auto uri = params["textDocument"].value("uri", "");
			auto hover = workspace.hover(uri, parse_position(params["position"]));
			if (!hover) respond(id, nullptr);
			else respond(id, {{"contents", {{"kind", "markdown"}, {"value", hover->markdown}}}, {"range", range_json(hover->range)}});
		} else if (method == "textDocument/definition") {
			auto uri = params["textDocument"].value("uri", "");
			auto locations = workspace.definition(uri, parse_position(params["position"]));
			json output = json::array();
			for (const auto &location : locations) output.push_back({{"uri", location.uri}, {"range", range_json(location.range)}});
			respond(id, std::move(output));
		} else if (method == "textDocument/documentSymbol") {
			auto uri = params["textDocument"].value("uri", "");
			json output = json::array();
			for (const auto &symbol : workspace.document_symbols(uri)) output.push_back(symbol_json(symbol));
			respond(id, std::move(output));
		} else if (method == "gdscript/resolveType") {
			auto uri = params["textDocument"].value("uri", "");
			auto type = workspace.resolve_type(uri, parse_position(params["position"]), params.value("expression", ""));
			respond(id, type_json(type));
		} else if (method == "gdscript/resolveExpression") {
			auto uri = params["textDocument"].value("uri", "");
			auto expression = workspace.resolve_expression(uri, parse_position(params["position"]),
				params.value("expression", ""));
			respond(id, expression_json(expression));
		} else if (method == "textDocument/diagnostic" || method == "gdscript/diagnostics") {
			auto uri = params["textDocument"].value("uri", "");
			json items = json::array();
			for (const auto &diagnostic : workspace.diagnostics(uri)) items.push_back(diagnostic_json(diagnostic));
			respond(id, {{"kind", "full"}, {"items", std::move(items)}});
		} else if (request) {
			respond_error(id, -32601, "Method not found: " + method);
		}
	}
	return 0;
}
