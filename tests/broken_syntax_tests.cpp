#include "core/document.hpp"
#include "core/text.hpp"
#include "core/workspace.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace gdscript_lsp;
using json = nlohmann::json;

namespace {

json position_json(Position p) { return {{"line", p.line}, {"character", p.character}}; }
json range_json(Range r) { return {{"start", position_json(r.start)}, {"end", position_json(r.end)}}; }
Position position(const json &p) { return {p.at("line"), p.at("character")}; }

void require(bool condition, const std::string &message) {
	if (!condition) throw std::runtime_error(message);
}

void check_range(Range range, Position end) {
	require(range.start <= range.end && range.end <= end, "invalid source range: " + range_json(range).dump());
}

json syntax_shape(const SyntaxNode &node, const Document &document) {
	check_range(node.range, byte_to_position(document.source(), document.source().size()));
	require(node.start_byte <= node.end_byte && node.end_byte <= document.source().size(), "invalid syntax byte offsets");
	require(node.range.start == byte_to_position(document.source(), node.start_byte) &&
		node.range.end == byte_to_position(document.source(), node.end_byte), "syntax UTF-16/byte position disagreement: " +
		std::string(node.kind) + " bytes=" + std::to_string(node.start_byte) + ":" + std::to_string(node.end_byte) +
		" range=" + range_json(node.range).dump());
	json children = json::array();
	for (const auto &child : node.children) children.push_back(syntax_shape(child, document));
	return json::array({node.kind, node.field, node.start_byte, node.end_byte, range_json(node.range), node.has_error, children});
}

json symbol_shape(const Symbol &symbol, Position end) {
	check_range(symbol.range, end);
	check_range(symbol.selection_range, end);
	json children = json::array();
	for (const auto &child : symbol.children) children.push_back(symbol_shape(child, end));
	return json::array({symbol.id, symbol.name, symbol.kind, symbol.declared_type, symbol.initializer, symbol.detail,
		range_json(symbol.range), range_json(symbol.selection_range), symbol.is_static, symbol.is_local,
		symbol.is_parameter, symbol.is_variadic, symbol.is_inferred, symbol.is_iteration_variable,
		symbol.malformed, symbol.body_recovered, children});
}

json document_shape(const Document &document) {
	json classes = json::array(), errors = json::array();
	auto end = byte_to_position(document.source(), document.source().size());
	for (const auto &record : document.classes()) {
		json members = json::array();
		for (const auto &member : record.members) members.push_back(symbol_shape(member, end));
		classes.push_back(json::array({record.global_name, record.extends_text, symbol_shape(record.symbol, end), members}));
	}
	for (const auto &error : document.syntax_errors()) {
		check_range(error.range, end);
		errors.push_back(json::array({range_json(error.range), error.message}));
	}
	return {{"syntax", syntax_shape(document.syntax_root(), document)}, {"classes", classes}, {"errors", errors}};
}

void flatten_outline(const std::vector<OutlineSymbol> &symbols, json &output) {
	for (const auto &symbol : symbols) {
		output.push_back({{"id", symbol.symbol_id}, {"owner", symbol.owner_id}, {"name", symbol.name},
			{"type", symbol.resolved_type.name}, {"return", symbol.return_type ? json(symbol.return_type->name) : json()},
			{"range", range_json(symbol.range)}, {"selection", range_json(symbol.selection_range)}});
		flatten_outline(symbol.children, output);
	}
}

json observe(Workspace &workspace, const std::string &uri, const json &snapshot) {
	json probes = json::array(), symbols = json::array(), diagnostics = json::array();
	for (const auto &probe : snapshot.at("probes")) {
		auto p = position(probe.at("position"));
		json result = json::object();
		if (probe.contains("members")) {
			std::vector<std::string> names;
			for (const auto &item : workspace.completion(uri, p)) names.push_back(item.filter_text.empty() ? item.label : item.filter_text);
			std::sort(names.begin(), names.end());
			result["completion"] = names;
		}
		if (probe.contains("expression")) result["type"] = workspace.resolve_type(uri, p, probe.at("expression")).name;
		if (probe.contains("target")) {
			result["definition"] = json::array();
			for (const auto &location : workspace.definition(uri, p)) {
				result["definition"].push_back({{"uri", location.uri}, {"range", range_json(location.range)}});
			}
			auto hover = workspace.hover(uri, p);
			result["hover"] = hover ? json{{"text", hover->markdown}, {"range", range_json(hover->range)}} : json();
		}
		probes.push_back(std::move(result));
	}
	flatten_outline(workspace.document_outline(uri).symbols, symbols);
	for (const auto &item : workspace.diagnostics(uri)) {
		diagnostics.push_back({{"code", item.code}, {"message", item.message}, {"range", range_json(item.range)}});
	}
	return {{"probes", probes}, {"symbols", symbols}, {"diagnostics", diagnostics}};
}

} // namespace

// The Python runner supplies one concrete edit sequence per subprocess. It
// applies the same semantic expectations to these observations and LSP results.
int main() {
	std::string active_source;
	size_t step = 0;
	try {
		json input;
		std::cin >> input;
		std::string root = input.at("root"), api = input.at("api"), uri = input.at("uri"), error;
		Workspace incremental;
		require(incremental.open(root, api, &error), "workspace open: " + error);
		std::unique_ptr<Document> previous;
		json observations = json::array();
		for (const auto &snapshot : input.at("snapshots")) {
			active_source = snapshot.at("source");
			auto version = static_cast<int64_t>(++step);
			auto parsed = previous ? std::make_unique<Document>(uri, "res://scenario.gd", active_source, version, *previous) :
				std::make_unique<Document>(uri, "res://scenario.gd", active_source, version);
			Document clean(uri, "res://scenario.gd", active_source, version);
			require(document_shape(*parsed) == document_shape(clean), "clean/incremental document mismatch");
			require(incremental.update_document(uri, active_source, version, &error), "incremental update: " + error);
			Workspace fresh;
			require(fresh.open(root, api, &error) && fresh.update_document(uri, active_source, version, &error), "fresh update: " + error);
			auto actual = observe(incremental, uri, snapshot);
			auto expected = observe(fresh, uri, snapshot);
			require(actual == expected, "clean/incremental semantic mismatch\nactual=" + actual.dump() + "\nfresh=" + expected.dump());
			observations.push_back(std::move(actual));
			previous = std::move(parsed);
		}
		std::cout << observations.dump() << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "step " << step << ": " << error.what() << "\nsource:\n" << active_source << '\n';
		return 1;
	}
}
