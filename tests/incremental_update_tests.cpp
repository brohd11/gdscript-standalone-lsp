#include "core/document.hpp"
#include "core/text.hpp"
#include "core/workspace.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace gdscript_lsp;

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

std::string read_file(const std::filesystem::path &path) {
	std::ifstream stream(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void append_position(std::vector<std::string> &shape, Position position) {
	shape.push_back(std::to_string(position.line) + ":" + std::to_string(position.character));
}

void append_symbol(std::vector<std::string> &shape, const Symbol &symbol) {
	shape.insert(shape.end(), {symbol.id, symbol.name, symbol.qualified_name, symbol.uri,
		std::to_string(static_cast<int>(symbol.kind)), symbol.declared_type, symbol.initializer, symbol.detail,
		symbol.is_static ? "1" : "0", symbol.is_local ? "1" : "0", symbol.is_parameter ? "1" : "0",
		symbol.is_variadic ? "1" : "0", symbol.is_inferred ? "1" : "0",
		symbol.is_iteration_variable ? "1" : "0", symbol.malformed ? "1" : "0"});
	shape.push_back(symbol.body_recovered ? "body-recovered" : "body-valid");
	append_position(shape, symbol.range.start);
	append_position(shape, symbol.range.end);
	append_position(shape, symbol.selection_range.start);
	append_position(shape, symbol.selection_range.end);
	for (const auto &child : symbol.children) append_symbol(shape, child);
	shape.push_back("end-symbol");
}

void append_syntax(std::vector<std::string> &shape, const SyntaxNode &node) {
	shape.insert(shape.end(), {std::string(node.kind), std::string(node.field), std::to_string(node.start_byte),
		std::to_string(node.end_byte), node.has_error ? "1" : "0"});
	append_position(shape, node.range.start);
	append_position(shape, node.range.end);
	for (const auto &child : node.children) append_syntax(shape, child);
	shape.push_back("end-node");
}

std::vector<std::string> document_shape(const Document &document) {
	std::vector<std::string> shape;
	append_syntax(shape, document.syntax_root());
	for (const auto &issue : document.syntax_errors()) {
		shape.push_back(issue.message);
		append_position(shape, issue.range.start);
		append_position(shape, issue.range.end);
	}
	for (const auto &record : document.classes()) {
		shape.insert(shape.end(), {record.global_name, record.extends_text});
		append_symbol(shape, record.symbol);
		for (const auto &member : record.members) append_symbol(shape, member);
		shape.push_back("end-class");
	}
	return shape;
}

bool contains(const std::vector<std::string> &values, const std::string &value) {
	return std::find(values.begin(), values.end(), value) != values.end();
}

const CompletionItem *find_item(const std::vector<CompletionItem> &items, const std::string &name) {
	auto found = std::find_if(items.begin(), items.end(), [&](const CompletionItem &item) {
		return (item.filter_text.empty() ? item.label : item.filter_text) == name;
	});
	return found == items.end() ? nullptr : &*found;
}

} // namespace

int main() {
	const std::string uri = "file:///incremental.gd";
	const std::string resource = "res://incremental.gd";
	std::vector<std::string> edits = {
		"extends RefCounted\n\nfunc inspect() -> void:\n\tvar title := \"hello\"\n\tprint(title)\n",
		"extends RefCounted\n\nfunc inspect() -> void:\n\tvar title: = \"world\"\n\tprint(title) # 😀\n",
		"extends RefCounted\n\nfunc inspect() -> void:\n\tvar dangling =\n\tprint(\"😀\")\n",
		"extends RefCounted\n\nfunc inspect() -> void:\n\tvar dangling:\n\tprint(\"😀\")\n",
		"extends RefCounted\n\nfunc inspect() -> void:\n\tvar values := [\n\t\t1,\n\t\t2,\n\t]\n\tprint(values)\n",
		"extends RefCounted\n\nfunc inspect() -> void:\n\tpass\n",
		"func test():\n\tvar n = get_nodes()\n\tn.\n\nfunc get_nodes() -> Node:\n\treturn Node.new()\n",
		"func get_nodes() -> Node:\n\treturn Node.new()\n\nfunc test():\n\tvar n = get_nodes()\n\tn.\n",
		"func get_nodes(\n",
		"func get_nodes()\n",
		"func get_nodes():\n",
		"func get_nodes() -> Node:\n\treturn Node.new()\n",
	};
	const std::string bounded_source =
		"# 😀 shifts later source locations\nclass Holder:\n"
		"\tfunc inspect():\n"
		"\t\tvar note = \"\"\"\nfunc fake():\nreturn 99\n\"\"\"\n"
		"\t\t# static func commented():\n"
		"\t\tvar values = [\n1,\n2,\n]\n"
		"\t\tvar n = make_node()\n\t\tn.\n\n"
		"\tstatic func make_node(\n\t\tlabel: String = \"😀\"\n\t) -> Node:\n"
		"\t\treturn Node.new()\n\n"
		"\tfunc later():\n\t\treturn 1\n";
	auto repaired_source = bounded_source;
	repaired_source.replace(repaired_source.find("n.\n"), 2, "n.get_name()");
	edits.insert(edits.end(), {bounded_source, repaired_source, bounded_source});
	auto previous = std::make_unique<Document>(uri, resource, edits.front(), 1);
	for (size_t index = 1; index < edits.size(); ++index) {
		auto incremental = std::make_unique<Document>(uri, resource, edits[index], index + 1, *previous);
		Document clean(uri, resource, edits[index], index + 1);
		expect(incremental->used_incremental_parse(), "replacement uses the previous tree");
		expect(document_shape(*incremental) == document_shape(clean),
			"incremental and clean parses have identical derived models at edit " + std::to_string(index));
		previous = std::move(incremental);
	}
	expect(previous->classes().size() == 2, "bounded recovery preserves the enclosing class");
	const auto *holder = previous->class_at(byte_to_position(bounded_source, bounded_source.find("var n")));
	expect(holder && holder->members.size() == 3 && std::none_of(holder->members.begin(), holder->members.end(),
		[](const Symbol &member) { return member.name == "fake" || member.name == "commented"; }),
		"multiline strings, grouped expressions, and comments do not introduce false function boundaries");
	auto *local = previous->find_local("n", byte_to_position(bounded_source, bounded_source.find("n.\n") + 2));
	expect(local && local->initializer == "make_node()", "locals after multiline literals remain in the function block");

	{
		auto spacing_fixture = std::filesystem::weakly_canonical("tests/fixtures/basic");
		Workspace spacing_workspace;
		std::string spacing_error;
		expect(spacing_workspace.open(spacing_fixture, spacing_fixture / "extension_api.json", &spacing_error),
			"inference-spacing workspace opens: " + spacing_error);
		auto spacing_uri = spacing_workspace.uri_for_path(spacing_fixture / "base.gd");
		auto conventional = read_file(spacing_fixture / "base.gd");
		conventional.insert(conventional.find('\n') + 1, "var inferred_spacing := {}\n");
		UpdateImpact spacing_impact;
		expect(spacing_workspace.update_document(spacing_uri, conventional, 2, &spacing_error, &spacing_impact),
			"conventional inference declaration updates");
		auto spaced = conventional;
		auto declaration_at = spaced.find("var inferred_spacing := {}");
		spaced.replace(declaration_at, std::string("var inferred_spacing := {}").size(),
			"var inferred_spacing: = {}");
		expect(spacing_workspace.update_document(spacing_uri, spaced, 3, &spacing_error, &spacing_impact),
			"spaced inference declaration updates");
		expect(spacing_impact.incremental_parse && !spacing_impact.full_rebuild,
			"spacing an inferred assignment remains on the incremental fast path");
		expect(spacing_workspace.diagnostics(spacing_uri).empty(),
			"spaced inferred member remains diagnostic-free after an incremental edit");
	}

	auto caret_fixture = std::filesystem::weakly_canonical("tests/fixtures/caret_completion");
	Workspace chain_workspace;
	std::string error;
	expect(chain_workspace.open(caret_fixture,
		std::filesystem::weakly_canonical("tests/fixtures/basic/extension_api.json"), &error),
		"member-chain workspace opens: " + error);
	auto chain_uri = chain_workspace.uri_for_path(caret_fixture / "main.gd");
	auto chain_prelude = std::string(
		"extends RefCounted\n\n"
		"const TF = ContextRoot.Utils.Profile.TimeFunction.TimeScale\n\n"
		"enum LocalState { IDLE, READY }\n"
		"enum OtherState { FIRST, SECOND }\n\n"
		"func consume(value: ContextRoot.Utils.Profile.TimeFunction.TimeScale) -> void:\n"
		"\tpass\n\n"
		"func inspect() -> void:\n\t");
	UpdateImpact chain_impact;
	auto update_chain = [&](std::string expression, int version) {
		auto source = chain_prelude + expression;
		expect(chain_workspace.update_document(chain_uri, source, version, &error, &chain_impact),
			"member-chain overlay updates: " + expression);
		return std::pair{std::move(source), chain_workspace.completion(chain_uri,
			byte_to_position(source, source.size()))};
	};
	update_chain("ContextRoot", 2);
	auto root_dot = update_chain("ContextRoot.", 3).second;
	expect(chain_impact.incremental_parse && !chain_impact.full_rebuild && find_item(root_dot, "Utils"),
		"a first trailing dot keeps the fast path and completes the root namespace");
	auto root_symbols = chain_workspace.document_symbols(chain_uri);
	auto inspect_symbol = root_symbols.empty() ? std::vector<Symbol>::const_iterator{} :
		std::find_if(root_symbols.front().children.begin(), root_symbols.front().children.end(),
			[](const Symbol &symbol) { return symbol.name == "inspect"; });
	expect(!root_symbols.empty() && inspect_symbol != root_symbols.front().children.end() &&
		inspect_symbol->declared_type == "void" && !inspect_symbol->malformed && inspect_symbol->body_recovered,
		"body recovery preserves the typed function header in document symbols");
	auto root_dot_diagnostics = chain_workspace.diagnostics(chain_uri);
	expect(std::any_of(root_dot_diagnostics.begin(), root_dot_diagnostics.end(),
		[](const Diagnostic &diagnostic) { return diagnostic.code == "syntax-error"; }),
		"the trailing dot still reports its syntax diagnostic");

	update_chain("ContextRoot.Utils", 4);
	auto utils_dot = update_chain("ContextRoot.Utils.", 5).second;
	expect(!chain_impact.full_rebuild && find_item(utils_dot, "Profile") && find_item(utils_dot, "helper"),
		"a nested trailing dot remains fast and resolves the nested namespace");
	update_chain("ContextRoot.Utils.Profile", 6);
	auto profile_dot = update_chain("ContextRoot.Utils.Profile.", 7).second;
	expect(!chain_impact.full_rebuild && find_item(profile_dot, "TimeFunction"),
		"a deep trailing dot remains fast and resolves the next namespace level");
	update_chain("ContextRoot.Utils.helper(", 8);
	expect(!chain_impact.full_rebuild, "an incomplete call in a valid function body remains on the fast path");

	auto inferred_prelude = chain_prelude +
		"ContextRoot\n\nfunc inferred_chain():\n\tvar callback = func(): return 1\n\treturn \"stable\"\n\t";
	auto inferred_valid = inferred_prelude + "ContextRoot";
	expect(chain_workspace.update_document(chain_uri, inferred_valid, 9, &error, &chain_impact),
		"inferred-chain baseline updates");
	expect(chain_impact.full_rebuild, "adding the inferred method is an exported topology change");
	auto inferred_error = inferred_prelude + "ContextRoot.";
	expect(chain_workspace.update_document(chain_uri, inferred_error, 10, &error, &chain_impact),
		"inferred-chain body error updates");
	expect(!chain_impact.full_rebuild, "body recovery retains an unannotated function's inferred return type");
	auto inferred_type = chain_workspace.resolve_type(chain_uri,
		byte_to_position(inferred_error, inferred_error.size()), "inferred_chain()");
	expect(inferred_type.kind == TypeKind::Builtin && inferred_type.name == "String",
		"recovered return inference ignores the nested lambda and retains String");
	auto incomplete_header = chain_prelude + "ContextRoot\n\nfunc inferred_chain(";
	expect(chain_workspace.update_document(chain_uri, incomplete_header, 11, &error, &chain_impact),
		"incomplete header overlay updates");
	expect(chain_impact.full_rebuild, "an actually incomplete function header retains the full fallback");

	auto fixture = std::filesystem::weakly_canonical("tests/fixtures/basic");
	Workspace workspace;
	expect(workspace.open(fixture, fixture / "extension_api.json", &error), "workspace opens: " + error);
	auto base_uri = workspace.uri_for_path(fixture / "base.gd");
	auto consumer_uri = workspace.uri_for_path(fixture / "consumer.gd");
	auto alias_uri = workspace.uri_for_path(fixture / "alias_derived.gd");
	auto child_uri = workspace.uri_for_path(fixture / "child.gd");
	auto original = read_file(fixture / "base.gd");
	auto source = original;
	auto return_at = source.find("return \"base\"");
	expect(return_at != std::string::npos, "base fixture return exists");
	source.replace(return_at, std::string("return \"base\"").size(), "return \"edited\"");
	UpdateImpact impact;
	expect(workspace.update_document(base_uri, source, 2, &error, &impact), "body-only update succeeds");
	expect(impact.incremental_parse && !impact.full_rebuild, "body-only update takes the incremental fast path");
	expect(impact.affected_documents == std::vector<std::string>{base_uri},
		"body-only update does not re-diagnose reverse dependents");
	auto completion = workspace.completion(consumer_uri, {6, 7});
	auto *label = find_item(completion, "label");
	expect(label != nullptr, "dependent completion survives document-local pointer replacement");
	expect(label && workspace.resolve_completion_item(label->symbol_id).has_value(),
		"completion resolution follows the replacement symbol pointer");

	source += "\nfunc inferred_value(value: int = 1):\n\treturn \"one\"\n";
	expect(workspace.update_document(base_uri, source, 3, &error, &impact), "new inferred method update succeeds");
	expect(impact.full_rebuild, "adding a method falls back to a full rebuild");
	auto same_type = source;
	auto one_at = same_type.rfind("\"one\"");
	same_type.replace(one_at, 5, "\"two\"");
	expect(workspace.update_document(base_uri, same_type, 4, &error, &impact), "same inferred return update succeeds");
	expect(impact.incremental_parse && !impact.full_rebuild,
		"an unannotated function body stays fast when its effective return type is unchanged");
	auto changed_type = same_type;
	auto two_at = changed_type.rfind("\"two\"");
	changed_type.replace(two_at, 5, "42");
	expect(workspace.update_document(base_uri, changed_type, 5, &error, &impact), "changed inferred return update succeeds");
	expect(impact.full_rebuild && contains(impact.affected_documents, consumer_uri),
		"a changed inferred return type rebuilds and invalidates dependents");
	auto inferred = workspace.resolve_type(consumer_uri, {6, 7}, "ChildThing.new().inferred_value()");
	expect(inferred.kind == TypeKind::Builtin && inferred.name == "int",
		"dependents observe the changed inferred return type");

	auto alias_dependency = changed_type;
	auto label_return = alias_dependency.find("return \"edited\"");
	alias_dependency.replace(label_return, std::string("return \"edited\"").size(),
		"var dependency = AliasDerived\n\treturn \"edited\"");
	expect(workspace.update_document(base_uri, alias_dependency, 6, &error, &impact),
		"body dependency update succeeds");
	expect(!impact.full_rebuild && contains(workspace.affected_documents({alias_uri}), base_uri),
		"fast path adds the edited document's new outgoing dependency");
	auto child_dependency = alias_dependency;
	auto alias_at = child_dependency.find("AliasDerived");
	child_dependency.replace(alias_at, std::string("AliasDerived").size(), "ChildThing");
	expect(workspace.update_document(base_uri, child_dependency, 7, &error, &impact),
		"body dependency replacement succeeds");
	expect(!impact.full_rebuild && !contains(workspace.affected_documents({alias_uri}), base_uri) &&
		contains(workspace.affected_documents({child_uri}), base_uri),
		"fast path removes the old reverse dependency and installs the new one");

	for (int version = 8; version < 18; ++version) {
		auto repeated = child_dependency;
		repeated.replace(repeated.find("\"edited\""), 8, version % 2 ? "\"odd\"" : "\"even\"");
		expect(workspace.update_document(base_uri, repeated, version, &error, &impact) && !impact.full_rebuild,
			"repeated body edit remains on the fast path");
		expect(find_item(workspace.completion(consumer_uri, {6, 7}), "label") != nullptr,
			"repeated replacement keeps dependent completion valid");
	}

	expect(workspace.update_document(base_uri, original + "\nvar added_surface: int\n", 18, &error, &impact),
		"member declaration update succeeds");
	expect(impact.full_rebuild, "member declaration changes retain the full rebuild fallback");
	expect(workspace.update_document(base_uri, original, 19, &error, &impact), "fixture restoration succeeds");
	expect(impact.full_rebuild, "removing a member declaration retains the full rebuild fallback");

	if (failures == 0) std::cout << "incremental update tests passed\n";
	return failures == 0 ? 0 : 1;
}
