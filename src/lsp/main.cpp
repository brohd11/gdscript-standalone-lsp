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
		{"filterText", item.filter_text.empty() ? item.label : item.filter_text}
	};
	if (!item.documentation.empty()) result["documentation"] = {{"kind", "markdown"}, {"value", item.documentation}};
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
		dirty_.clear();
		full_pending_ = false;
		condition_.notify_all();
		return generation_;
	}

	void finish_update(uint64_t generation, const std::string &uri) {
		std::lock_guard lock(mutex_);
		if (generation != generation_) return;
		dirty_.insert(uri);
		full_pending_ = true;
		last_change_ = std::chrono::steady_clock::now();
		condition_.notify_all();
	}

	void schedule_full() {
		std::lock_guard lock(mutex_);
		++generation_;
		dirty_.clear();
		full_pending_ = true;
		last_change_ = std::chrono::steady_clock::now();
		condition_.notify_all();
	}

	void stop() {
		if (!worker_.joinable()) return;
		worker_.request_stop();
		condition_.notify_all();
		worker_.join();
	}

private:
	static constexpr auto full_scan_delay_ = std::chrono::milliseconds(200);

	bool publish(const std::string &uri, uint64_t generation, bool force, std::stop_token stop) {
		json items = json::array();
		for (const auto &diagnostic : workspace_.diagnostics(uri)) items.push_back(diagnostic_json(diagnostic));
		auto version = workspace_.document_version(uri);
		auto cache_key = items.dump();

		std::lock_guard lock(mutex_);
		if (stop.stop_requested() || generation != generation_) return false;
		auto previous = published_.find(uri);
		if (!force && previous != published_.end() && previous->second == cache_key) return true;
		published_[uri] = std::move(cache_key);
		json params = {{"uri", uri}, {"diagnostics", std::move(items)}};
		if (version >= 0) params["version"] = version;
		send({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", std::move(params)}});
		return true;
	}

	void run(std::stop_token stop) {
		while (!stop.stop_requested()) {
			std::vector<std::string> dirty;
			uint64_t generation = 0;
			bool full_scan = false;
			{
				std::unique_lock lock(mutex_);
				condition_.wait(lock, [&] {
					return stop.stop_requested() || !dirty_.empty() || full_pending_;
				});
				if (stop.stop_requested()) return;
				generation = generation_;
				if (!dirty_.empty()) {
					dirty.assign(dirty_.begin(), dirty_.end());
					dirty_.clear();
				} else {
					auto deadline = last_change_ + full_scan_delay_;
					if (condition_.wait_until(lock, deadline, [&] {
						return stop.stop_requested() || generation_ != generation || !dirty_.empty();
					})) continue;
					full_pending_ = false;
					full_scan = true;
				}
			}
			if (!dirty.empty()) {
				for (const auto &uri : dirty) {
					if (!publish(uri, generation, true, stop)) break;
				}
				continue;
			}
			if (full_scan) {
				for (const auto &uri : workspace_.document_uris()) {
					if (!publish(uri, generation, false, stop)) break;
				}
			}
		}
	}

	Workspace &workspace_;
	std::mutex mutex_;
	std::condition_variable condition_;
	std::unordered_set<std::string> dirty_;
	std::unordered_map<std::string, std::string> published_;
	uint64_t generation_ = 0;
	bool full_pending_ = false;
	std::chrono::steady_clock::time_point last_change_ = std::chrono::steady_clock::now();
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
			{"completionProvider", {{"triggerCharacters", json::array({"."})}, {"resolveProvider", false}}},
			{"hoverProvider", true},
			{"definitionProvider", true},
			{"documentSymbolProvider", true},
			{"diagnosticProvider", {{"identifier", "gdscript-lsp"}, {"interFileDependencies", true},
				{"workspaceDiagnostics", false}}}
		}},
		{"serverInfo", {{"name", "gdscript-lsp"}, {"version", "0.1.0"}}}
	};
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
			diagnostics.stop();
			respond(id, nullptr);
		} else if (method == "textDocument/didOpen") {
			auto document = params["textDocument"];
			auto uri = document.value("uri", "");
			auto text = document.value("text", "");
			buffers[uri] = text;
			auto generation = diagnostics.begin_update();
			if (workspace.update_document(uri, std::move(text), document.value("version", -1), &error)) {
				diagnostics.finish_update(generation, uri);
			}
		} else if (method == "textDocument/didChange") {
			auto document = params["textDocument"];
			auto uri = document.value("uri", "");
			if (!buffers.contains(uri)) buffers[uri] = "";
			apply_content_changes(buffers[uri], params.value("contentChanges", json::array()));
			auto generation = diagnostics.begin_update();
			if (workspace.update_document(uri, buffers[uri], document.value("version", -1), &error)) {
				diagnostics.finish_update(generation, uri);
			}
		} else if (method == "textDocument/didClose") {
			auto uri = params["textDocument"].value("uri", "");
			buffers.erase(uri);
			auto generation = diagnostics.begin_update();
			if (workspace.close_document(uri, &error)) diagnostics.finish_update(generation, uri);
		} else if (method == "workspace/didChangeWatchedFiles") {
			diagnostics.begin_update();
			for (const auto &change : params.value("changes", json::array())) {
				auto uri = change.value("uri", "");
				if (!buffers.contains(uri)) workspace.refresh_file(uri, &error);
			}
			diagnostics.schedule_full();
		} else if (method == "textDocument/completion") {
			auto uri = params["textDocument"].value("uri", "");
			auto items = workspace.completion(uri, parse_position(params["position"]));
			json output = json::array();
			for (const auto &item : items) output.push_back(completion_json(item));
			respond(id, {{"isIncomplete", false}, {"items", std::move(output)}});
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
