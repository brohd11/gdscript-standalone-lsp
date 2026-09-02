#include "core/text.hpp"
#include "core/workspace.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
		{"insertText", item.insert_text.empty() ? item.label : item.insert_text}
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
	auto body = message.dump();
	std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
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

} // namespace

int main(int argc, char **argv) {
	std::filesystem::path project;
	std::filesystem::path api;
	for (int index = 1; index < argc; ++index) {
		std::string argument = argv[index];
		if (argument == "--project" && index + 1 < argc) project = argv[++index];
		else if (argument == "--api" && index + 1 < argc) api = argv[++index];
		else if (argument == "--version") {
			std::cout << "gdscript-lsp 0.1.0 (Godot 4.6)\n";
			return 0;
		}
	}
	if (project.empty()) {
		std::cerr << "usage: gdscript-lsp --project <path> [--api <extension_api.json>]\n";
		return 2;
	}
	if (api.empty()) {
		if (const char *environment_api = std::getenv("GDSCRIPT_LSP_API")) api = environment_api;
	}
	if (api.empty()) {
		for (const auto &candidate : {
				project / ".godot/addons/gdscript_parser/extension_api.json",
				project / "addons/gdscript_lsp/data/godot-4.6-extension-api.json"}) {
			if (std::filesystem::exists(candidate)) {
				api = candidate;
				break;
			}
		}
	}
	if (api.empty()) {
		std::error_code ec;
		auto executable = std::filesystem::weakly_canonical(argv[0], ec);
		for (const auto &candidate : {
				executable.parent_path().parent_path() / "addons/gdscript_lsp/data/godot-4.6-extension-api.json",
				executable.parent_path().parent_path() / "share/gdscript-lsp/godot-4.6-extension-api.json"}) {
			if (std::filesystem::exists(candidate)) {
				api = candidate;
				break;
			}
		}
	}

	Workspace workspace;
	std::string error;
	if (!workspace.open(project, api, &error)) {
		std::cerr << "gdscript-lsp: " << error << '\n';
		return 1;
	}
	const auto &stats = workspace.stats();
	std::cerr << "gdscript-lsp: indexed " << stats.document_count << " documents / " << stats.class_count
			  << " classes in " << stats.elapsed_ms << " ms; syntax errors: " << stats.syntax_error_count << '\n';

	std::unordered_map<std::string, std::string> buffers;
	std::unordered_set<std::string> cancelled;
	bool shutdown = false;
	auto publish_diagnostics = [&](const std::string &uri) {
		json items = json::array();
		for (const auto &diagnostic : workspace.diagnostics(uri)) items.push_back(diagnostic_json(diagnostic));
		json params = {{"uri", uri}, {"diagnostics", std::move(items)}};
		auto version = workspace.document_version(uri);
		if (version >= 0) params["version"] = version;
		send({{"jsonrpc", "2.0"}, {"method", "textDocument/publishDiagnostics"}, {"params", std::move(params)}});
	};
	auto publish_all_diagnostics = [&] {
		for (const auto &uri : workspace.document_uris()) publish_diagnostics(uri);
	};
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
			respond(id, {
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
			});
		} else if (method == "initialized") {
			publish_all_diagnostics();
		} else if (method == "shutdown") {
			shutdown = true;
			respond(id, nullptr);
		} else if (method == "exit") {
			return shutdown ? 0 : 1;
		} else if (method == "textDocument/didOpen") {
			auto document = params["textDocument"];
			auto uri = document.value("uri", "");
			auto text = document.value("text", "");
			buffers[uri] = text;
			workspace.update_document(uri, std::move(text), document.value("version", -1), &error);
			publish_all_diagnostics();
		} else if (method == "textDocument/didChange") {
			auto document = params["textDocument"];
			auto uri = document.value("uri", "");
			if (!buffers.contains(uri)) buffers[uri] = "";
			apply_content_changes(buffers[uri], params.value("contentChanges", json::array()));
			workspace.update_document(uri, buffers[uri], document.value("version", -1), &error);
			publish_all_diagnostics();
		} else if (method == "textDocument/didClose") {
			auto uri = params["textDocument"].value("uri", "");
			buffers.erase(uri);
			workspace.close_document(uri, &error);
			publish_all_diagnostics();
		} else if (method == "workspace/didChangeWatchedFiles") {
			for (const auto &change : params.value("changes", json::array())) {
				auto uri = change.value("uri", "");
				if (!buffers.contains(uri)) workspace.refresh_file(uri, &error);
			}
			publish_all_diagnostics();
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
