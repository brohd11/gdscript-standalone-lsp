#include "core/text.hpp"
#include "core/uri.hpp"
#include "core/workspace.hpp"
#include "lsp/tcp_adapter.hpp"
#include "lsp/diagnostic_coordinator.hpp"
#include "lsp/engine_transport.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <charconv>
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

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

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

json signature_help_json(const SignatureHelpResult &help) {
	json signatures = json::array();
	for (const auto &signature : help.signatures) {
		json parameters = json::array();
		for (const auto &parameter : signature.parameters) {
			json value = {{"label", json::array({parameter.label_start, parameter.label_end})}};
			if (!parameter.documentation.empty()) {
				value["documentation"] = {{"kind", "markdown"}, {"value", parameter.documentation}};
			}
			parameters.push_back(std::move(value));
		}
		json value = {{"label", signature.label}, {"parameters", std::move(parameters)}};
		if (!signature.documentation.empty()) {
			value["documentation"] = {{"kind", "markdown"}, {"value", signature.documentation}};
		}
		if (signature.active_parameter) value["activeParameter"] = *signature.active_parameter;
		signatures.push_back(std::move(value));
	}
	json result = {{"signatures", std::move(signatures)}, {"activeSignature", help.active_signature}};
	if (help.active_parameter) result["activeParameter"] = *help.active_parameter;
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

json outline_symbol_json(const OutlineSymbol &symbol) {
	json children = json::array();
	for (const auto &child : symbol.children) children.push_back(outline_symbol_json(child));
	json result = {
		{"symbolId", symbol.symbol_id}, {"ownerId", symbol.owner_id},
		{"name", symbol.name}, {"detail", symbol.detail},
		{"kind", static_cast<int>(symbol.kind)}, {"range", range_json(symbol.range)},
		{"selectionRange", range_json(symbol.selection_range)},
		{"resolvedType", type_json(symbol.resolved_type)}, {"returnType", nullptr},
		{"origin", nullptr},
		{"flags", {
			{"static", symbol.is_static}, {"staticTyped", symbol.static_typed},
			{"inferred", symbol.inferred}, {"local", symbol.is_local},
			{"parameter", symbol.is_parameter}, {"variadic", symbol.is_variadic},
			{"malformed", symbol.malformed}
		}}
	};
	if (symbol.return_type) result["returnType"] = type_json(*symbol.return_type);
	if (symbol.origin) result["origin"] = origin_json(*symbol.origin);
	if (!children.empty()) result["children"] = std::move(children);
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

std::string canonical_document_uri(std::string uri) {
	return canonical_file_uri(uri).value_or(std::move(uri));
}


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

constexpr std::string_view completion_prefixes = ".(,:=\"'";

json initialize_result(bool space_prefix) {
	auto trigger_characters = json::array({".", "(", ",", ":", "=", "\"", "'"});
	if (space_prefix) trigger_characters.push_back(" ");
	return {
		{"capabilities", {
			{"positionEncoding", "utf-16"},
			{"textDocumentSync", {{"openClose", true}, {"change", 2}, {"save", {{"includeText", false}}}}},
			{"completionProvider", {{"triggerCharacters", std::move(trigger_characters)},
				{"resolveProvider", true}}},
			{"signatureHelpProvider", {{"triggerCharacters", json::array({"(", ","})},
				{"retriggerCharacters", json::array({","})}}},
			{"hoverProvider", true},
			{"definitionProvider", true},
			{"documentSymbolProvider", true},
			{"diagnosticProvider", {{"identifier", "gdscript-lsp"}, {"interFileDependencies", true},
				{"workspaceDiagnostics", false}}}
		}},
		{"serverInfo", {{"name", "gdscript-lsp"}, {"version", "0.1.0"}}}
	};
}

bool is_space_completion_trigger(const json &params) {
	auto context = params.find("context");
	return context != params.end() && context->is_object() &&
		context->value("triggerKind", 0) == 2 && context->value("triggerCharacter", "") == " ";
}

bool follows_completion_prefix(std::string_view source, Position position) {
	auto offset = position_to_byte(source, position);
	return offset >= 2 && source[offset - 1] == ' ' &&
		completion_prefixes.find(source[offset - 2]) != std::string_view::npos;
}

std::vector<std::string> merge_uris(std::vector<std::string> first, const std::vector<std::string> &second) {
	first.insert(first.end(), second.begin(), second.end());
	std::sort(first.begin(), first.end());
	first.erase(std::unique(first.begin(), first.end()), first.end());
	return first;
}

void apply_configuration(Workspace &workspace, DiagnosticCoordinator &publisher, const json &settings,
		const std::optional<EngineConfiguration> &cli_engine = std::nullopt) {
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
		if (!cli_engine && diagnostics->contains("engine")) publisher.configure_engine(EngineConfiguration::parse((*diagnostics)["engine"]));
		if (auto interval = diagnostics->find("pollIntervalMs"); interval != diagnostics->end() && interval->is_number_integer()) {
			auto milliseconds = interval->get<int64_t>();
			if (milliseconds > 0) milliseconds = std::max<int64_t>(milliseconds, 100);
			publisher.set_poll_interval(std::chrono::milliseconds(std::max<int64_t>(milliseconds, 0)));
		}
	}
	if (cli_engine) publisher.configure_engine(*cli_engine);
}

} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	std::filesystem::path project;
	std::filesystem::path configured_api;
	std::optional<uint16_t> tcp_port;
	std::optional<EngineConfiguration> cli_engine;
	bool space_prefix = false;
	for (int index = 1; index < argc; ++index) {
		std::string argument = argv[index];
		if (argument == "--project" && index + 1 < argc) project = argv[++index];
		else if (argument == "--api" && index + 1 < argc) configured_api = argv[++index];
		else if (argument == "--space-prefix") space_prefix = true;
		else if (argument == "--godot" || argument == "--godot-lsp-port") {
			if (cli_engine || index + 1 >= argc) {
				std::cerr << "gdscript-lsp: --godot and --godot-lsp-port require a value and are mutually exclusive\n"; return 2;
			}
			std::string value = argv[++index];
			try {
				if (argument == "--godot") cli_engine = EngineConfiguration::parse({{"mode", "launch"}, {"executable", value}});
				else {
					int port = 0; auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
					if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size()) throw std::invalid_argument("Invalid engine LSP port");
					cli_engine = EngineConfiguration::parse({{"mode", "attach"}, {"port", port}});
				}
			} catch (const std::exception &error) { std::cerr << "gdscript-lsp: " << error.what() << '\n'; return 2; }
		}
		else if (argument == "--tcp") {
			if (tcp_port || index + 1 >= argc) {
				std::cerr << "gdscript-lsp: --tcp requires one port\n";
				return 2;
			}
			std::string_view value = argv[++index];
			uint32_t parsed = 0;
			auto [end, error_code] = std::from_chars(value.data(), value.data() + value.size(), parsed);
			if (error_code != std::errc() || end != value.data() + value.size() || parsed == 0 || parsed > 65535) {
				std::cerr << "gdscript-lsp: invalid TCP port: " << value << '\n';
				return 2;
			}
			tcp_port = static_cast<uint16_t>(parsed);
		}
		else if (argument == "--version") {
			std::cout << "gdscript-lsp 0.1.0 (Godot 4.6)\n";
			return 0;
		}
	}
	if (tcp_port) return run_tcp_adapter(*tcp_port, argc, argv);
	install_engine_termination_handlers();
	Workspace workspace;
	DiagnosticCoordinator diagnostics(workspace, send, diagnostic_json);
	if (cli_engine) diagnostics.configure_engine(*cli_engine);
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
			diagnostics.cancel_pull(params.value("id", json(nullptr)));
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
				try { apply_configuration(workspace, diagnostics, *options, cli_engine); }
				catch (const std::exception &error) { respond_error(id, -32602, error.what()); continue; }
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
			respond(id, initialize_result(space_prefix));
		} else if (method == "exit") {
			return shutdown ? 0 : 1;
		} else if (!initialized) {
			if (request) respond_error(id, -32002, "Server not initialized");
		} else if (method == "initialized") {
			diagnostics.start_engine();
			diagnostics.schedule_full();
		} else if (method == "shutdown") {
			shutdown = true;
			diagnostics.request_stop();
			respond(id, nullptr);
		} else if (method == "textDocument/didOpen") {
			auto document = params["textDocument"];
			auto client_uri = document.value("uri", "");
			auto uri = canonical_document_uri(client_uri);
			auto text = document.value("text", "");
			buffers[uri] = text;
			diagnostics.set_open(uri, true, client_uri);
			auto generation = diagnostics.begin_update();
			UpdateImpact impact;
			if (workspace.update_document(uri, std::move(text), document.value("version", -1), &error, &impact)) {
				diagnostics.finish_update(generation, {uri}, impact.affected_documents);
			} else diagnostics.set_open(uri, false);
		} else if (method == "textDocument/didChange") {
			auto document = params["textDocument"];
			auto uri = canonical_document_uri(document.value("uri", ""));
			if (!buffers.contains(uri)) buffers[uri] = "";
			apply_content_changes(buffers[uri], params.value("contentChanges", json::array()));
			auto generation = diagnostics.begin_update();
			UpdateImpact impact;
			if (workspace.update_document(uri, buffers[uri], document.value("version", -1), &error, &impact)) {
				diagnostics.finish_update(generation, {uri}, impact.affected_documents);
			}
		} else if (method == "textDocument/didClose") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			buffers.erase(uri);
			diagnostics.set_open(uri, false);
			auto generation = diagnostics.begin_update();
			auto affected = workspace.affected_documents({uri});
			if (workspace.close_document(uri, &error)) {
				affected = merge_uris(std::move(affected), workspace.affected_documents({uri}));
				diagnostics.finish_update(generation, {uri}, affected);
			}
		} else if (method == "textDocument/didSave") {
			diagnostics.saved(canonical_document_uri(params["textDocument"].value("uri", "")));
		} else if (method == "workspace/didChangeConfiguration") {
			try { apply_configuration(workspace, diagnostics, params.value("settings", json::object()), cli_engine); }
			catch (const std::exception &error) { send({{"jsonrpc", "2.0"}, {"method", "window/logMessage"}, {"params", {{"type", 1}, {"message", error.what()}}}}); }
		} else if (method == "workspace/didChangeWatchedFiles") {
			auto generation = diagnostics.begin_update();
			std::vector<std::string> changed;
			for (const auto &change : params.value("changes", json::array())) {
				auto uri = canonical_document_uri(change.value("uri", ""));
				if (!buffers.contains(uri)) changed.push_back(uri);
			}
			auto affected = workspace.affected_documents(changed);
			for (const auto &uri : changed) workspace.refresh_file(uri, &error);
			diagnostics.external_changes(changed);
			affected = merge_uris(std::move(affected), workspace.affected_documents(changed));
			diagnostics.finish_update(generation, {}, affected);
		} else if (method == "textDocument/completion") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			auto position = parse_position(params["position"]);
			auto buffer = buffers.find(uri);
			if (is_space_completion_trigger(params) && (!space_prefix || buffer == buffers.end() ||
					!follows_completion_prefix(buffer->second, position))) {
				respond(id, {{"isIncomplete", false}, {"items", json::array()}});
			} else {
				auto completion = workspace.completion_result(uri, position);
				json output = json::array();
				for (const auto &item : completion.items) output.push_back(completion_json(item));
				respond(id, {{"isIncomplete", completion.is_incomplete}, {"items", std::move(output)}});
			}
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
		} else if (method == "textDocument/signatureHelp") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			auto help = workspace.signature_help(uri, parse_position(params["position"]));
			if (!help) respond(id, nullptr);
			else respond(id, signature_help_json(*help));
		} else if (method == "textDocument/hover") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			auto hover = workspace.hover(uri, parse_position(params["position"]));
			if (!hover) respond(id, nullptr);
			else respond(id, {{"contents", {{"kind", "markdown"}, {"value", hover->markdown}}}, {"range", range_json(hover->range)}});
		} else if (method == "textDocument/definition") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			auto locations = workspace.definition(uri, parse_position(params["position"]));
			json output = json::array();
			for (const auto &location : locations) output.push_back({{"uri", location.uri}, {"range", range_json(location.range)}});
			respond(id, std::move(output));
		} else if (method == "textDocument/documentSymbol") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			json output = json::array();
			for (const auto &symbol : workspace.document_symbols(uri)) output.push_back(symbol_json(symbol));
			respond(id, std::move(output));
		} else if (method == "gdscript/documentSymbols") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			auto outline = workspace.document_outline(uri);
			json symbols = json::array();
			for (const auto &symbol : outline.symbols) symbols.push_back(outline_symbol_json(symbol));
			respond(id, {{"version", outline.version}, {"symbols", std::move(symbols)}});
		} else if (method == "gdscript/resolveType") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			auto type = workspace.resolve_type(uri, parse_position(params["position"]), params.value("expression", ""));
			respond(id, type_json(type));
		} else if (method == "gdscript/resolveExpression") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			auto expression = workspace.resolve_expression(uri, parse_position(params["position"]),
				params.value("expression", ""));
			respond(id, expression_json(expression));
		} else if (method == "textDocument/diagnostic" || method == "gdscript/diagnostics") {
			auto uri = canonical_document_uri(params["textDocument"].value("uri", ""));
			diagnostics.pull(uri, id);
		} else if (method == "gdscript/diagnosticBackend") {
			respond(id, diagnostics.engine_status());
		} else if (method == "gdscript/reconnectDiagnosticEngine") {
			diagnostics.reconnect_engine();
			if (request) respond(id, nullptr);
		} else if (request) {
			respond_error(id, -32601, "Method not found: " + method);
		}
	}
	return 0;
}
