#include "core/gdscript_api.hpp"
#include "core/text.hpp"
#include "core/uri.hpp"
#include "core/workspace.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

using namespace gdscript_lsp;

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

bool has_item(const std::vector<CompletionItem> &items, const std::string &name) {
	return std::any_of(items.begin(), items.end(), [&](const auto &item) {
		return (item.filter_text.empty() ? item.label : item.filter_text) == name;
	});
}

const CompletionItem *find_item(const std::vector<CompletionItem> &items, const std::string &name) {
	auto found = std::find_if(items.begin(), items.end(), [&](const auto &item) {
		return (item.filter_text.empty() ? item.label : item.filter_text) == name;
	});
	return found == items.end() ? nullptr : &*found;
}

size_t item_index(const std::vector<CompletionItem> &items, const std::string &name) {
	for (size_t index = 0; index < items.size(); ++index) {
		if ((items[index].filter_text.empty() ? items[index].label : items[index].filter_text) == name) return index;
	}
	return items.size();
}

bool completion_ranks_increase(const std::vector<CompletionItem> &items) {
	for (size_t index = 1; index < items.size(); ++index) {
		if (items[index - 1].sort_text >= items[index].sort_text) return false;
	}
	return !items.empty() && !items.front().sort_text.empty();
}

bool has_diagnostic(const std::vector<Diagnostic> &items, const std::string &code) {
	return std::any_of(items.begin(), items.end(), [&](const auto &item) { return item.code == code; });
}

size_t diagnostic_count(const std::vector<Diagnostic> &items, const std::string &code) {
	return static_cast<size_t>(std::count_if(items.begin(), items.end(),
		[&](const auto &item) { return item.code == code; }));
}

const Diagnostic *find_diagnostic(const std::vector<Diagnostic> &items, const std::string &code) {
	auto found = std::find_if(items.begin(), items.end(),
		[&](const auto &item) { return item.code == code; });
	return found == items.end() ? nullptr : &*found;
}

} // namespace

int main() {
	auto fixture = std::filesystem::weakly_canonical("tests/fixtures/basic");
	Workspace workspace;
	std::string error;
	expect(workspace.open(fixture, fixture / "extension_api.json", &error), "workspace opens: " + error);
	expect(workspace.stats().document_count == 12, "all fixture scripts indexed");
	expect(workspace.native_api().version() == "4.6.3", "native API version loaded");

	auto consumer_uri = workspace.uri_for_path(fixture / "consumer.gd");
	{
		Workspace leading_newline_workspace;
		expect(leading_newline_workspace.open(fixture, fixture / "extension_api.json", &error),
			"leading-newline completion workspace opens: " + error);
		auto leading_uri = leading_newline_workspace.uri_for_path(fixture / "consumer.gd");
		std::string leading_source =
			"\nextends RefCounted\n\nfunc inspect() -> void:\n\tvar class_obj := {}\n\tc\n";
		expect(leading_newline_workspace.update_document(leading_uri, leading_source, 41, &error),
			"leading-newline completion overlay accepted");
		auto leading_completion = leading_newline_workspace.completion(leading_uri, {5, 2});
		expect(has_item(leading_completion, "class_obj"),
			"scope completion terminates and retains locals when byte zero is a newline");
	}
	auto completion = workspace.completion(consumer_uri, {6, 7});
	expect(has_item(completion, "own"), "completion includes direct script member");
	expect(has_item(completion, "count"), "completion includes inherited script member");
	expect(has_item(completion, "label"), "completion includes inherited method");
	expect(has_item(completion, "reference_method"), "completion includes transitive native member");
	expect(item_index(completion, "own") < item_index(completion, "CHILD_CONSTANT") &&
		item_index(completion, "CHILD_CONSTANT") < item_index(completion, "count") &&
		item_index(completion, "count") < item_index(completion, "BASE_CONSTANT") &&
		item_index(completion, "BASE_CONSTANT") < item_index(completion, "reference_method") &&
		item_index(completion, "native_takes") < item_index(completion, "ref_static") &&
		item_index(completion, "ref_static") < item_index(completion, "get_class") &&
		item_index(completion, "get_class") < item_index(completion, "object_static"),
		"instance completion walks derived-to-base and moves type-level members behind each level");
	auto unqualified_completion = workspace.completion(consumer_uri, {6, 6});
	expect(item_index(unqualified_completion, "local") < item_index(unqualified_completion, "child"),
		"visible locals rank ahead of current-class members");
	expect(static_cast<size_t>(std::count_if(completion.begin(), completion.end(), [](const CompletionItem &item) {
		return item.filter_text == "shared_name";
	})) == 1, "derived completion member suppresses the inherited member with the same name");
	expect(completion_ranks_increase(completion), "completion sort ranks preserve server relevance order");

	auto class_completion = workspace.completion(consumer_uri, {9, 14});
	expect(has_item(class_completion, "CHILD_CONSTANT") && has_item(class_completion, "child_static") &&
		has_item(class_completion, "BASE_CONSTANT") && has_item(class_completion, "base_static") &&
		!has_item(class_completion, "own") && !has_item(class_completion, "make_base") &&
		!has_item(class_completion, "count") && !has_item(class_completion, "label"),
		"script class receiver offers inherited type-level members and omits instance members");
	expect(item_index(class_completion, "CHILD_CONSTANT") < item_index(class_completion, "child_static") &&
		item_index(class_completion, "child_static") < item_index(class_completion, "BASE_CONSTANT") &&
		item_index(class_completion, "BASE_CONSTANT") < item_index(class_completion, "base_static"),
		"class receiver preserves source order within each nearest-first inheritance level");

	auto child_uri = workspace.uri_for_path(fixture / "child.gd");
	auto static_context_completion = workspace.completion(child_uri, {6, 2});
	expect(has_item(static_context_completion, "CHILD_CONSTANT") && has_item(static_context_completion, "base_static") &&
		has_item(static_context_completion, "ref_static") && !has_item(static_context_completion, "own") &&
		!has_item(static_context_completion, "label") && !has_item(static_context_completion, "reference_method"),
		"unqualified completion in a static function omits script and native instance members");

	auto native_instance_members = workspace.native_api().members("RefCounted");
	std::vector<std::string> native_instance_names;
	for (auto *member : native_instance_members) native_instance_names.push_back(member->name);
	expect(native_instance_names == std::vector<std::string>({
		"reference_method", "native_takes", "ref_static", "get_class", "object_static"}),
		"native members preserve API order, static tails, and nearest-first inheritance");
	auto native_type_members = workspace.native_api().members("RefCounted", MemberAccess::Type);
	std::vector<std::string> native_type_names;
	for (auto *member : native_type_members) native_type_names.push_back(member->name);
	expect(native_type_names == std::vector<std::string>({"ref_static", "object_static"}),
		"native class access contains only type-level members");

	auto local_type = workspace.resolve_type(consumer_uri, {6, 4}, "local");
	expect(local_type.kind == TypeKind::ScriptClass && local_type.name == "ChildThing", "typed local resolves to script class");
	auto child_type = workspace.resolve_type(consumer_uri, {2, 6}, "child");
	expect(child_type.kind == TypeKind::ScriptClass && child_type.name == "ChildThing", "constructor inference resolves script instance");
	auto autoload_type = workspace.resolve_type(consumer_uri, {6, 4}, "FixtureGlobal");
	expect(autoload_type.kind == TypeKind::ScriptClass && autoload_type.instance, "autoload resolves as script instance");
	auto inferred_uri = workspace.uri_for_path(fixture / "return_inference.gd");
	auto factory_uri = workspace.uri_for_path(fixture / "return_factory.gd");
	auto factory_affected = workspace.affected_documents({factory_uri});
	expect(std::find(factory_affected.begin(), factory_affected.end(), inferred_uri) != factory_affected.end() &&
		std::find(factory_affected.begin(), factory_affected.end(), consumer_uri) == factory_affected.end(),
		"dependency closure includes script-path consumers without invalidating unrelated documents");
	auto inferred_type = workspace.resolve_type(inferred_uri, {8, 2}, "item");
	expect(inferred_type.kind == TypeKind::ScriptClass && inferred_type.name == "Product" && inferred_type.instance,
		"ordinary variable receives a non-binding hint from a qualified script call return type");
	auto inferred_completion = workspace.completion(inferred_uri, {7, 6});
	expect(has_item(inferred_completion, "product_member"),
		"qualified script call return hint drives member completion");
	auto alias_completion_hint = workspace.completion(inferred_uri, {10, 12});
	expect(has_item(alias_completion_hint, "product_member"),
		"static class alias call return hint drives member completion");
	auto direct_call_completion = workspace.completion(inferred_uri, {11, 26});
	expect(has_item(direct_call_completion, "product_member") && !has_item(direct_call_completion, "inspect"),
		"completion recursively resolves a qualified call result without leaking current-script members");
	auto nested_property_completion = workspace.completion(inferred_uri, {12, 36});
	expect(has_item(nested_property_completion, "keys") && has_item(nested_property_completion, "size") &&
		!has_item(nested_property_completion, "inspect"),
		"completion recursively resolves a property on a qualified call result");
	auto self_completion = workspace.completion(inferred_uri, {13, 6});
	expect(has_item(self_completion, "inspect"), "resolved self member access retains current-script completion");
	auto subscript_completion = workspace.completion(inferred_uri, {15, 13});
	expect(has_item(subscript_completion, "product_member"),
		"typed array subscript carries its element type into member completion");
	auto parenthesized_completion = workspace.completion(inferred_uri, {16, 28});
	expect(has_item(parenthesized_completion, "product_member"),
		"parenthesized call chain carries its result into member completion");
	auto multiline_completion = workspace.completion(inferred_uri, {18, 3});
	expect(has_item(multiline_completion, "product_member"),
		"multiline call chain carries its result into member completion");
	auto inferred_hover = workspace.hover(inferred_uri, {8, 2});
	expect(inferred_hover && inferred_hover->markdown.find("Inferred value type: `Product`") != std::string::npos,
		"hover distinguishes an initializer type hint from a declared type");
	expect(workspace.diagnostics(inferred_uri).empty(),
		"an ordinary variable remains dynamically assignable after receiving an initializer hint");
	std::ifstream chain_stream(fixture / "return_inference.gd");
	std::string chain_source{std::istreambuf_iterator<char>(chain_stream), std::istreambuf_iterator<char>()};
	auto chain_expression = std::string("Namespace.Factory.make().functions.keys()");
	auto chain_offset = chain_source.find(chain_expression);
	expect(chain_offset != std::string::npos, "recursive postfix completion fixture expression exists");
	if (chain_offset != std::string::npos) chain_source.insert(chain_offset + chain_expression.size(), ".");
	expect(workspace.update_document(inferred_uri, chain_source, 8, &error),
		"recursive postfix completion overlay accepted");
	auto repeated_call_completion = workspace.completion(inferred_uri, {12, 43});
	expect(has_item(repeated_call_completion, "append_array") && !has_item(repeated_call_completion, "keys") &&
		!has_item(repeated_call_completion, "inspect"),
		"completion resolves calls and members repeatedly through the final Array value");
	expect(workspace.close_document(inferred_uri, &error), "recursive postfix completion overlay closes");
	const std::string unresolved_member_source =
		"extends RefCounted\n\nfunc inspect() -> void:\n"
		"\tins.missing\n\tins().missing\n\tins(.missing\n";
	expect(workspace.update_document(inferred_uri, unresolved_member_source, 9, &error),
		"unresolved member completion overlay accepted");
	for (auto position : {Position{3, 5}, Position{4, 7}, Position{5, 6}}) {
		auto unresolved_completion = workspace.completion(inferred_uri, position);
		expect(unresolved_completion.empty(),
			"unresolved or malformed member access does not fall back to unqualified completion");
	}
	expect(workspace.close_document(inferred_uri, &error), "unresolved member completion overlay closes");

	auto definitions = workspace.definition(consumer_uri, {2, 15});
	expect(definitions.size() == 1 && definitions.front().uri.ends_with("/child.gd"), "global class definition resolves");
	auto symbols = workspace.document_symbols(consumer_uri);
	expect(!symbols.empty() && symbols.front().children.size() == 3, "document symbols include members");

	auto alias_uri = workspace.uri_for_path(fixture / "alias_derived.gd");
	expect(workspace.diagnostics(alias_uri).empty(),
		"qualified script aliases provide inherited members, aliases, and enums");
	auto alias_completion = workspace.completion(alias_uri, {5, 1});
	expect(has_item(alias_completion, "inherited_alias_member") && has_item(alias_completion, "Imported") &&
		has_item(alias_completion, "ExitCode"), "completion includes members inherited through a qualified alias");
	auto namespace_uri = workspace.uri_for_path(fixture / "alias_namespace.gd");
	expect(workspace.diagnostics(namespace_uri).empty(),
		"physical inner classes and inner classes extending an outer alias resolve");
	auto bridge_uri = workspace.uri_for_path(fixture / "alias_bridge.gd");
	expect(workspace.update_document(bridge_uri, "const BaseAlias = preload(\"res://base.gd\")\n", 3, &error),
		"script alias overlay accepted");
	alias_completion = workspace.completion(alias_uri, {5, 1});
	expect(has_item(alias_completion, "label") && !has_item(alias_completion, "inherited_alias_member"),
		"changing an alias overlay rebuilds dependent inheritance");
	expect(workspace.close_document(bridge_uri, &error), "script alias overlay closes");

	auto unicode = std::string("a😀b\nvalue");
	auto byte = position_to_byte(unicode, {0, 3});
	expect(byte == 5, "UTF-16 position converts surrogate pair");
	expect(byte_to_position(unicode, byte) == Position{0, 3}, "byte converts back to UTF-16 position");
	auto encoded_path = std::filesystem::path("/tmp/gdscript lsp/project.godot");
	auto encoded_uri = file_uri_for_path(encoded_path);
	expect(encoded_uri.find("gdscript%20lsp") != std::string::npos, "file URI percent-encodes spaces");
	expect(path_for_file_uri(encoded_uri) == std::filesystem::absolute(encoded_path).lexically_normal(),
		"file URI round trips to a path");
	expect(path_for_file_uri("file://localhost/tmp/project.godot") == std::filesystem::path("/tmp/project.godot"),
		"localhost file URI is accepted");
	expect(!path_for_file_uri("file://remote-host/tmp/project.godot"), "remote file URI authority is rejected");
	expect(!path_for_file_uri("file:///tmp/bad%2"), "malformed percent escape is rejected");

	auto changed = std::string(
		"class_name ChildThing extends BaseThing\n\n"
		"var own := \"child\"\n"
		"var added: int = 2\n\n"
		"func make_base() -> BaseThing:\n\treturn self\n");
	expect(workspace.update_document(child_uri, changed, 2, &error), "unsaved document update accepted");
	completion = workspace.completion(consumer_uri, {6, 7});
	expect(has_item(completion, "added"), "dependent completion reflects unsaved base update");
	expect(workspace.close_document(child_uri, &error), "closing overlay restores disk");
	completion = workspace.completion(consumer_uri, {6, 7});
	expect(!has_item(completion, "added"), "closing overlay invalidates dependent result");

	auto outside_uri = workspace.uri_for_path(fixture.parent_path() / "basic-sibling/escape.gd");
	expect(!workspace.update_document(outside_uri, "extends Node\n", 1, &error),
		"document overlays cannot escape the workspace by path prefix");
	Workspace invalid_api_workspace;
	expect(!invalid_api_workspace.open(fixture, fixture / "missing-extension-api.json", &error),
		"an explicitly requested missing native API is reported");

	auto diagnostic_fixture = std::filesystem::weakly_canonical("tests/fixtures/diagnostics");
	Workspace diagnostic_workspace;
	expect(diagnostic_workspace.open(diagnostic_fixture, fixture / "extension_api.json", &error),
		"diagnostic workspace opens: " + error);
	auto diagnostic_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "errors.gd");
	const std::string helper_source =
		"extends RefCounted\n\n"
		"enum State { IDLE, READY }\n"
		"class Product:\n"
		"\tvar title: String\n"
		"\tvar _private: int\n"
		"\tfunc _init(required: int) -> void: pass\n"
		"\tfunc build(value: int) -> void: pass\n\n"
		"func accepts(state: State) -> void: pass\n\n"
		"func inspect(target: Product) -> void:\n"
		"\tvar state: State = State.IDLE\n"
		"\taccepts(State.IDLE)\n"
		"\tvar product: Product = Product.new()\n"
		"\ttarget.call(\"build\")\n"
		"\ttarget.set(\"title\", \"value\")\n"
		"\tvar typed: Product\n"
		"\tprint(target.title)\n"
		"\tvar ordinary = 1\n";
	expect(diagnostic_workspace.update_document(diagnostic_uri, helper_source, 100, &error),
		"completion helper overlay accepted");
	auto helper_position = [&](std::string_view marker) {
		auto found = helper_source.find(marker);
		expect(found != std::string::npos, "completion helper marker exists");
		return byte_to_position(helper_source, found + marker.size());
	};
	auto enum_result = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("var state: State = "), CompletionProfile::Helpers);
	expect(enum_result.disposition == CompletionDisposition::Replace && enum_result.provider == "enums" &&
		has_item(enum_result.items, "State.IDLE") && has_item(enum_result.items, "State.READY"),
		"enum helper owns an expected script-enum assignment");
	auto enum_argument = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("accepts("), CompletionProfile::Helpers);
	expect(enum_argument.disposition == CompletionDisposition::Replace && has_item(enum_argument.items, "State.IDLE"),
		"enum helper resolves a script function argument type");
	auto constructor_result = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("var product: Product = "), CompletionProfile::Helpers);
	auto *constructor = find_item(constructor_result.items, "Product.new");
	expect(constructor_result.disposition == CompletionDisposition::Augment && constructor &&
		constructor->insert_text == "Product.new(", "constructor helper preserves Godot's argument-aware insertion");
	auto method_string = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("target.call(\""), CompletionProfile::Helpers);
	expect(method_string.disposition == CompletionDisposition::Replace && method_string.provider == "memberStrings" &&
		has_item(method_string.items, "build") && !has_item(method_string.items, "title"),
		"member-string helper owns call() and offers methods only");
	auto property_string = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("target.set(\""), CompletionProfile::Helpers);
	expect(property_string.disposition == CompletionDisposition::Replace && has_item(property_string.items, "title") &&
		!has_item(property_string.items, "build"), "member-string helper offers properties for set()");
	auto type_hint_result = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("var typed: "), CompletionProfile::Helpers);
	expect(type_hint_result.disposition == CompletionDisposition::Augment && has_item(type_hint_result.items, "Product") &&
		has_item(type_hint_result.items, "State") && has_item(type_hint_result.items, "RefCounted"),
		"extended type-hint helper includes local, enum, and native types");
	auto member_result = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("print(target."), CompletionProfile::Full);
	expect(has_item(member_result.items, "title") && !has_item(member_result.items, "_private"),
		"full completion hides private members until an underscore is typed");
	auto ordinary_helpers = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("var ordinary = "), CompletionProfile::Helpers);
	expect(ordinary_helpers.disposition == CompletionDisposition::NotHandled && ordinary_helpers.items.empty(),
		"helper profile leaves ordinary completion untouched");
	auto helper_config = diagnostic_workspace.completion_config();
	helper_config.enums = false;
	diagnostic_workspace.set_completion_config(helper_config);
	enum_result = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("var state: State = "), CompletionProfile::Helpers);
	expect(enum_result.disposition != CompletionDisposition::Replace,
		"completion provider configuration applies without reindexing");
	helper_config.enums = true;
	diagnostic_workspace.set_completion_config(helper_config);
	expect(diagnostic_workspace.close_document(diagnostic_uri, &error), "completion helper overlay closes");
	auto diagnostics = diagnostic_workspace.diagnostics(diagnostic_uri);
	if (diagnostic_count(diagnostics, "type-mismatch") != 2) {
		for (const auto &item : diagnostics) std::cerr << "diagnostic: " << item.code << ": " << item.message << '\n';
	}
	expect(has_diagnostic(diagnostics, "duplicate-symbol"), "duplicate member is diagnosed");
	expect(diagnostic_count(diagnostics, "unknown-type") == 2, "unknown member and parameter types are diagnosed");
	expect(diagnostic_count(diagnostics, "type-mismatch") == 2, "member and local assignment mismatches are diagnosed");
	auto duplicate = std::find_if(diagnostics.begin(), diagnostics.end(),
		[](const auto &item) { return item.code == "duplicate-symbol"; });
	expect(duplicate != diagnostics.end() && duplicate->related_information.size() == 1,
		"duplicate diagnostic points to the first declaration");

	auto unresolved_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "unresolved.gd");
	auto unresolved_diagnostics = diagnostic_workspace.diagnostics(unresolved_uri);
	expect(has_diagnostic(unresolved_diagnostics, "unresolved-base"),
		"unresolved base class is diagnosed");
	expect(has_diagnostic(unresolved_diagnostics, "unknown-type") &&
		has_diagnostic(unresolved_diagnostics, "undefined-function") &&
		has_diagnostic(unresolved_diagnostics, "undefined-identifier"),
		"an unresolved base does not suppress downstream semantic diagnostics");
	for (const auto &name : {"cycle_a.gd", "cycle_b.gd"}) {
		auto uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / name);
		expect(has_diagnostic(diagnostic_workspace.diagnostics(uri), "inheritance-cycle"),
			std::string("inheritance cycle is diagnosed in ") + name);
	}
	for (const auto &name : {"duplicate_a.gd", "duplicate_b.gd"}) {
		auto uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / name);
		expect(has_diagnostic(diagnostic_workspace.diagnostics(uri), "duplicate-global-class"),
			std::string("duplicate global class is diagnosed in ") + name);
	}
	auto syntax_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "syntax_error.gd");
	expect(has_diagnostic(diagnostic_workspace.diagnostics(syntax_uri), "syntax-error"), "syntax error is diagnosed");
	auto reserved_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "reserved_identifier.gd");
	auto reserved_diagnostics = diagnostic_workspace.diagnostics(reserved_uri);
	auto *reserved = find_diagnostic(reserved_diagnostics, "syntax-error");
	expect(reserved && reserved->message == R"(Expected variable name after "var".)" &&
		reserved->range.start == Position{3, 5} && reserved->range.end == Position{3, 10},
		"reserved variable name receives Godot's parser message on the identifier range");
	expect(gdscript_reserved_words().size() == 44 && is_gdscript_reserved_identifier("class") &&
		is_gdscript_reserved_identifier("while") && is_gdscript_reserved_identifier("true") &&
		is_gdscript_reserved_identifier("PI") && is_gdscript_reserved_identifier("_") &&
		!is_gdscript_reserved_identifier("String") && !is_gdscript_reserved_identifier("Node") &&
		!is_gdscript_reserved_identifier("print") && !is_gdscript_reserved_identifier("get") &&
		!is_gdscript_reserved_identifier("set"),
		"reserved identifiers match Godot 4.6 without banning shadowable API names");
	auto valid_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "valid_child.gd");
	expect(diagnostic_workspace.diagnostics(valid_uri).empty(),
		"assigning a derived script instance to its base type is valid");

	const std::string fixed = "extends RefCounted\n\nvar value: int = 1\n";
	expect(diagnostic_workspace.update_document(diagnostic_uri, fixed, 7, &error), "diagnostic overlay accepted");
	expect(diagnostic_workspace.diagnostics(diagnostic_uri).empty(), "fixed overlay clears diagnostics");
	expect(diagnostic_workspace.document_version(diagnostic_uri) == 7, "diagnostics expose overlay version");
	expect(diagnostic_workspace.close_document(diagnostic_uri, &error), "diagnostic overlay closes");
	expect(has_diagnostic(diagnostic_workspace.diagnostics(diagnostic_uri), "duplicate-symbol"),
		"closing overlay restores disk diagnostics");

	auto semantic_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "semantic_errors.gd");
	auto semantic_diagnostics = diagnostic_workspace.diagnostics(semantic_uri);
	const std::unordered_map<std::string, size_t> expected_semantic = {
		{"undefined-identifier", 2}, {"undefined-function", 1}, {"unknown-member", 1},
		{"not-callable", 1}, {"argument-count", 2}, {"argument-type", 2},
		{"missing-return-path", 1}, {"return-value-in-void", 2},
		{"missing-return-value", 1}, {"return-type-mismatch", 3},
	};
	for (const auto &[code, count] : expected_semantic) {
		expect(diagnostic_count(semantic_diagnostics, code) == count,
			"semantic diagnostic count for " + code + " is " + std::to_string(count));
	}
	if (semantic_diagnostics.size() != 16) {
		for (const auto &item : semantic_diagnostics) std::cerr << "semantic diagnostic: " << item.code << ": " << item.message << '\n';
	}
	expect(semantic_diagnostics.size() == 16, "semantic fixture has no unexpected diagnostics");
	auto semantic_valid_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "semantic_valid.gd");
	auto semantic_valid_diagnostics = diagnostic_workspace.diagnostics(semantic_valid_uri);
	if (!semantic_valid_diagnostics.empty()) {
		for (const auto &item : semantic_valid_diagnostics) std::cerr << "valid semantic diagnostic: " << item.code << ": " << item.message << '\n';
	}
	expect(semantic_valid_diagnostics.empty(),
		"dynamic receivers, utilities, singletons, derived arguments, and complete returns are valid");

	auto script_completion = diagnostic_workspace.completion(semantic_valid_uri, {28, 1});
	auto *zero_argument_script = find_item(script_completion, "nullable_return");
	expect(zero_argument_script && zero_argument_script->label == "nullable_return()" &&
		zero_argument_script->insert_text == "nullable_return()" && zero_argument_script->detail.empty(),
		"zero-argument script completion uses Godot's compact display and complete call insertion");
	auto *argument_script = find_item(script_completion, "accepts_base");
	expect(argument_script && argument_script->label == "accepts_base(\xe2\x80\xa6)" &&
		argument_script->insert_text == "accepts_base(" && argument_script->filter_text == "accepts_base" &&
		argument_script->detail.empty(),
		"parameterized script completion displays an ellipsis and inserts a trailing opener");
	auto *variadic_script = find_item(script_completion, "variadic");
	expect(variadic_script && variadic_script->label == "variadic(\xe2\x80\xa6)" &&
		variadic_script->insert_text == "variadic(", "variadic script completion is parameterized");

	auto callable_completion = diagnostic_workspace.completion(semantic_valid_uri, {46, 20});
	auto *variadic_native = find_item(callable_completion, "call");
	expect(variadic_native && variadic_native->label == "call(\xe2\x80\xa6)" &&
		variadic_native->insert_text == "call(", "variadic native completion is parameterized");
	auto dictionary_completion = diagnostic_workspace.completion(semantic_valid_uri, {30, 6});
	auto *zero_argument_native = find_item(dictionary_completion, "keys");
	expect(zero_argument_native && zero_argument_native->label == "keys()" &&
		zero_argument_native->insert_text == "keys()", "zero-argument native completion inserts a complete call");
	auto array_completion = diagnostic_workspace.completion(semantic_valid_uri, {51, 16});
	auto *argument_native = find_item(array_completion, "append_array");
	expect(argument_native && argument_native->label == "append_array(\xe2\x80\xa6)" &&
		argument_native->insert_text == "append_array(", "parameterized native completion inserts a trailing opener");
	auto singleton_completion = diagnostic_workspace.completion(semantic_valid_uri, {32, 8});
	expect(has_item(singleton_completion, "get_version_info"),
		"native singleton completion retains instance members despite its class-like identifier");
	auto native_class_completion = diagnostic_workspace.completion(semantic_valid_uri, {52, 22});
	expect(has_item(native_class_completion, "ModeFlags") && has_item(native_class_completion, "READ") &&
		!has_item(native_class_completion, "reference_method"),
		"native class receiver exposes type-level members and omits inherited instance methods");

	auto warning_fixture = std::filesystem::temp_directory_path() /
		("gdscript-lsp-warning-fixture-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(warning_fixture);
	auto write_warning_settings = [&](const std::string &settings) {
		std::ofstream stream(warning_fixture / "project.godot");
		stream << settings;
	};
	write_warning_settings(
		"[application]\nconfig/name=\"Warning fixture\"\n\n"
		"[debug]\n"
		"gdscript/warnings/unsafe_property_access=1\n"
		"gdscript/warnings/unsafe_method_access=1\n");
	{
		std::ofstream stream(warning_fixture / "warnings.gd");
		stream <<
			"extends RefCounted\n\n"
			"const SELF_SCRIPT = preload(\"res://warnings.gd\")\n"
			"var typed_dictionary: Dictionary[String, Dictionary] = {}\n"
			"var typed_array: Array[String] = []\n\n"
			"func accepts_script(value: Script) -> void:\n\tprint(value)\n"
			"func accepts_dictionary(value: Dictionary) -> void:\n\tprint(value)\n"
			"func accepts_array(value: Array) -> void:\n\tprint(value)\n\n"
			"func inspect(node: Node, singleton_name: String) -> void:\n"
			"\taccepts_script(SELF_SCRIPT)\n"
			"\ttyped_dictionary.clear()\n"
			"\tvar dictionary_keys := typed_dictionary.keys()\n"
			"\taccepts_dictionary(typed_dictionary)\n"
			"\ttyped_array.append(\"value\")\n"
			"\taccepts_array(typed_array)\n"
			"\tnode.custom_property\n"
			"\tnode.custom_method()\n"
			"\t\"text\".custom_property\n"
			"\tis_instance_of(node, Node)\n"
			"\tis_instance_of(node)\n"
			"\tEngine.get_singleton(\"EditorInterface\").get_edited_scene_root()\n"
			"\tEngine.get_singleton(singleton_name).custom_method()\n";
	}
	Workspace warning_workspace;
	expect(warning_workspace.open(warning_fixture,
		std::filesystem::weakly_canonical("addons/gdscript_lsp/data/godot-4.6-extension-api.json"), &error),
		"warning fixture opens: " + error);
	auto warning_uri = warning_workspace.uri_for_path(warning_fixture / "warnings.gd");
	auto warning_diagnostics = warning_workspace.diagnostics(warning_uri);
	if (diagnostic_count(warning_diagnostics, "unsafe-property-access") != 1 ||
			diagnostic_count(warning_diagnostics, "unsafe-method-access") != 2) {
		for (const auto &item : warning_diagnostics) {
			std::cerr << "warning fixture: " << item.code << ": " << item.message << '\n';
		}
	}
	expect(diagnostic_count(warning_diagnostics, "unsafe-property-access") == 1 &&
		diagnostic_count(warning_diagnostics, "unsafe-method-access") == 2,
		"unsafe instance members use distinct property and method diagnostics");
	expect(std::all_of(warning_diagnostics.begin(), warning_diagnostics.end(), [](const Diagnostic &diagnostic) {
		return !diagnostic.code.starts_with("unsafe-") || diagnostic.severity == DiagnosticSeverity::Warning;
	}), "project warning level 1 maps to LSP warning severity");
	expect(diagnostic_count(warning_diagnostics, "unknown-member") == 1,
		"missing builtin members remain hard errors");
	expect(diagnostic_count(warning_diagnostics, "argument-count") == 1,
		"GDScript-specific is_instance_of signature validates arity");
	expect(diagnostic_count(warning_diagnostics, "argument-type") == 0,
		"script resources and typed containers are accepted by their native base types");
	expect(warning_diagnostics.size() == 5, "typed containers and script resources add no unexpected diagnostics");
	auto typed_array_type = warning_workspace.resolve_type(warning_uri, {15, 1}, "typed_array");
	expect(typed_array_type.kind == TypeKind::Builtin && typed_array_type.name == "Array" &&
		typed_array_type.arguments.size() == 1 && typed_array_type.display() == "Array[String]",
		"typed arrays retain arguments while using the Array base type");

	write_warning_settings(
		"[debug]\n"
		"gdscript/warnings/unsafe_property_access=2\n"
		"gdscript/warnings/unsafe_method_access=2\n");
	auto project_uri = warning_workspace.uri_for_path(warning_fixture / "project.godot");
	expect(warning_workspace.refresh_file(project_uri, &error), "project warning settings refresh");
	warning_diagnostics = warning_workspace.diagnostics(warning_uri);
	expect(std::all_of(warning_diagnostics.begin(), warning_diagnostics.end(), [](const Diagnostic &diagnostic) {
		return !diagnostic.code.starts_with("unsafe-") || diagnostic.severity == DiagnosticSeverity::Error;
	}), "project warning level 2 maps to LSP error severity");
	write_warning_settings("[application]\nconfig/name=\"Warning fixture\"\n");
	expect(warning_workspace.refresh_file(project_uri, &error), "removed warning settings refresh");
	warning_diagnostics = warning_workspace.diagnostics(warning_uri);
	expect(diagnostic_count(warning_diagnostics, "unsafe-property-access") == 0 &&
		diagnostic_count(warning_diagnostics, "unsafe-method-access") == 0,
		"missing unsafe settings restore Godot's disabled defaults");
	std::filesystem::remove_all(warning_fixture);

	Workspace legacy_api_workspace;
	auto legacy_api = std::filesystem::weakly_canonical("tests/fixtures/legacy_extension_api.json");
	expect(legacy_api_workspace.open(diagnostic_fixture, legacy_api, &error), "legacy reduced API workspace opens: " + error);
	auto legacy_call_uri = legacy_api_workspace.uri_for_path(diagnostic_fixture / "legacy_api_call.gd");
	expect(legacy_api_workspace.diagnostics(legacy_call_uri).empty(),
		"missing legacy default/vararg metadata does not create native arity errors");

	// This table mirrors tests_plugin/brohd/gdscript_parser/inference_test.gd.
	// It intentionally lives in an isolated native fixture so the copied plugin
	// tests can keep their original res:// layout and UIDs untouched.
	Workspace inference_workspace;
	auto inference_fixture = std::filesystem::weakly_canonical("tests/fixtures/inference");
	auto inference_api = std::filesystem::weakly_canonical("addons/gdscript_lsp/data/godot-4.6-extension-api.json");
	expect(inference_workspace.open(inference_fixture, inference_api, &error),
		"inference corpus workspace opens: " + error);
	auto inference_uri = inference_workspace.uri_for_path(inference_fixture / "scenario.gd");
	struct InferenceCase { const char *name; uint32_t line; const char *expected; };
	const std::vector<InferenceCase> inference_cases = {
		{"explicit_int", 6, "int"}, {"dict_key", 7, "String"}, {"dict_value", 9, "int"},
		{"color_val", 11, "Color"}, {"chan_by_str", 12, "float"}, {"chan_by_idx", 13, "float"},
		{"html_str", 14, "String"}, {"first_char", 15, "String"}, {"lambda_ref", 16, "Callable"},
		{"awaited_signal_arg", 17, "int"}, {"signal_ref", 18, "Signal"}, {"awaited_ref", 19, "int"},
		{"builtin_callable", 20, "Callable"}, {"builtin_ret", 21, "String"},
		{"callable_chain_bool", 22, "bool"}, {"returned_callable", 23, "Callable"},
		{"called_signal", 24, "Signal"}, {"awaited_return", 25, "String"},
		{"returned_callable2", 26, "Callable"}, {"called_signal2", 27, "Signal"},
		{"awaited_bool", 28, "bool"}, {"sig_connections", 29, "Array"},
		{"typed_conns", 30, "Array[String]"}, {"made_obj", 31, "InferenceSupport"},
		{"obj_string", 32, "String"}, {"static_string", 33, "String"},
		{"subscript_new", 34, "InferenceSupport"}, {"subscript_string", 35, "String"},
		{"made_signal", 36, "Signal"}, {"awaited_made", 37, "bool"},
		{"got_variant", 39, "String"}, {"menu_callable", 40, "Callable"},
		{"menu", 41, "PopupMenu"}, {"awaited_text", 42, "String"},
		{"typed_map", 43, "Dictionary[InferenceEnum, Nested]"},
		{"map_key", 44, "InferenceEnum"}, {"map_val", 45, "Nested"},
		{"packed", 47, "PackedByteArray"}, {"byte", 48, "int"},
		{"preload_const", 50, "Color"}, {"cast_obj", 51, "InferenceEnum"},
		{"nested_callable", 52, "Callable"}, {"nested_call_ret", 53, "ProcessMode"},
		{"nested_direct", 54, "ProcessMode"}, {"node_ins", 55, "Node"}, {"node_static", 56, "Node"},
		{"pre_shadow_callable", 57, "Callable"}, {"shadowed_func", 58, "String"},
		{"post_shadow", 59, "String"}, {"direct_terminal", 80, "Color"},
	};
	for (const auto &test : inference_cases) {
		auto type = inference_workspace.resolve_type(inference_uri, {test.line, 2}, test.name);
		expect(type.display() == test.expected,
			std::string("inference corpus ") + test.name + " expected " + test.expected + " got " + type.display());
	}
	auto node_instance = inference_workspace.resolve_type(inference_uri, {55, 2}, "node_ins");
	auto node_type_value = inference_workspace.resolve_type(inference_uri, {56, 2}, "node_static");
	expect(node_instance.instance && !node_type_value.instance,
		"inference corpus distinguishes constructed native instances from bare type values");
	auto inference_diagnostics = inference_workspace.diagnostics(inference_uri);
	if (!inference_diagnostics.empty()) for (const auto &item : inference_diagnostics) {
		std::cerr << "inference scenario:" << item.range.start.line + 1 << ':' << item.range.start.character + 1 << ": "
			<< item.code << ": " << item.message << '\n';
	}
	expect(inference_diagnostics.empty(), "inference corpus introduces no semantic false positives");

	Workspace repository_workspace;
	auto repository_api = std::filesystem::weakly_canonical("addons/gdscript_lsp/data/godot-4.6-extension-api.json");
	expect(repository_workspace.open(".", repository_api, &error), "repository workspace opens: " + error);
	auto u_file_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/addon_lib/brohd/alib_runtime/utils/u_file.gd"));
	auto u_file_diagnostics = repository_workspace.diagnostics(u_file_uri);
	if (!u_file_diagnostics.empty()) for (const auto &item : u_file_diagnostics) {
		std::cerr << "u_file.gd:" << item.range.start.line + 1 << ':' << item.range.start.character + 1 << ": "
			<< item.code << ": " << item.message << '\n';
	}
	expect(u_file_diagnostics.empty(),
		"u_file.gd does not statically narrow ordinary Variant variables");
	auto command_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/editor_console/src/default_commands/script/script.gd"));
	expect(repository_workspace.diagnostics(command_uri).empty(),
		"qualified EditorConsoleSingleton base resolves with all inherited command members");
	for (const auto &path : {
			"addons/addon_lib/brohd/popup_wrapper/popup_wrapper.gd",
			"addons/addon_lib/brohd/alib_runtime/utils/u_node.gd",
			"addons/addon_lib/brohd/dock_manager/dock_manager.gd",
			"addons/code_completions/src/class/editor_code_completion_singleton.gd",
			"addons/addon_lib/brohd/alib_runtime/utils/gdscript/parser/utils/code_edit_parser.gd",
			"addons/syntax_plus/src/highlighter/highlighter_logic.gd",
			"addons/addon_lib/brohd/alib_editor/file_system/fs_tab/filesystem_tab.gd",
			"addons/addon_lib/brohd/alib_editor/misc/scene_viewer/scene_viewer.gd",
			"addons/editor_console/src/container/line_edit.gd",
			"addons/addon_lib/brohd/alib_editor/misc/git_service/git_data_draw.gd",
			"addons/addon_lib/brohd/collections/class/collection_button.gd",
			"addons/gdscript_lsp/editor/native_completion.gd",
			"addons/gdscript_lsp/plugin.gd"}) {
		auto uri = repository_workspace.uri_for_path(std::filesystem::weakly_canonical(path));
		auto reported = repository_workspace.diagnostics(uri);
		if (!reported.empty()) {
			for (const auto &item : reported) {
				std::cerr << path << ": " << item.code << ": " << item.message << '\n';
			}
		}
		expect(reported.empty(), std::string(path) + " has no false diagnostics");
	}
	auto popup_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/addon_lib/brohd/popup_wrapper/popup_wrapper.gd"));
	auto position_type = repository_workspace.resolve_type(popup_uri, {413, 30}, "ItemParams.Position.TOP");
	expect(position_type.kind == TypeKind::Builtin && position_type.name == "int",
		"qualified nested enum values resolve as integers");
	auto enum_completion = repository_workspace.completion(popup_uri, {413, 39});
	expect(has_item(enum_completion, "TOP") && has_item(enum_completion, "BOTTOM") && has_item(enum_completion, "keys"),
		"qualified enum completion includes values and Dictionary methods");
	auto u_node_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/addon_lib/brohd/alib_runtime/utils/u_node.gd"));
	expect(has_item(repository_workspace.completion(u_node_uri, {28, 2}), "is_instance_of"),
		"GDScript-specific builtins are offered in completion");
	auto global_completion = repository_workspace.completion(u_node_uri, {28, 2});
	expect(has_item(global_completion, "len") && has_item(global_completion, "char"),
		"all modeled GDScript builtins are offered in completion");
	auto *builtin_completion = find_item(global_completion, "is_instance_of");
	expect(builtin_completion && builtin_completion->label == "is_instance_of(\xe2\x80\xa6)" &&
		builtin_completion->filter_text == "is_instance_of" && builtin_completion->insert_text == "is_instance_of(",
		"GDScript builtin completion uses compact display, bare filtering, and trailing-opener insertion");
	auto dock_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/addon_lib/brohd/dock_manager/dock_manager.gd"));
	auto dock_symbols = repository_workspace.document_symbols(dock_uri);
	bool found_dock_constructor = false;
	for (const auto &record : dock_symbols) for (const auto &member : record.children) {
		if (member.kind == SymbolKind::Constructor && member.name == "_init" && member.children.size() == 5) {
			found_dock_constructor = true;
		}
	}
	expect(found_dock_constructor, "multiline _init is indexed with its complete constructor signature");
	auto code_completion_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/code_completions/src/class/editor_code_completion_singleton.gd"));
	auto script_resource_type = repository_workspace.resolve_type(code_completion_uri, {53, 30}, "PE_STRIP_CAST_SCRIPT");
	expect(script_resource_type.kind == TypeKind::ScriptClass && !script_resource_type.instance,
		"preloaded scripts remain class objects when used as values");
	auto code_edit_uri = repository_workspace.uri_for_path(std::filesystem::weakly_canonical(
		"addons/addon_lib/brohd/alib_runtime/utils/gdscript/parser/utils/code_edit_parser.gd"));
	auto path_constant_type = repository_workspace.resolve_type(code_edit_uri, {407, 42}, "TREE_SITTER_MANAGER_PATH");
	expect(path_constant_type.kind == TypeKind::Builtin && path_constant_type.name == "String",
		"quoted resource-path constants remain strings");
	auto highlighter_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/syntax_plus/src/highlighter/highlighter_logic.gd"));
	auto typed_dictionary_type = repository_workspace.resolve_type(highlighter_uri, {134, 20}, "func_arg_highlighters");
	expect(typed_dictionary_type.kind == TypeKind::Builtin && typed_dictionary_type.name == "Dictionary" &&
		typed_dictionary_type.arguments.size() == 2 && typed_dictionary_type.display() == "Dictionary[String, Dictionary]",
		"typed dictionaries retain arguments while using the Dictionary base type");
	auto yaml_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/addon_lib/yaml_parser/yaml.gd"));
	expect(has_item(repository_workspace.completion(yaml_uri, {823, 8}), "depth"),
		"completion keeps initializer hints for ordinary Variant variables without making them statically typed");
	auto scene_resource_uri = repository_workspace.uri_for_path(std::filesystem::weakly_canonical(
		"addons/addon_lib/brohd/dock_manager/dock_popup/dock_popup_handler.gd"));
	auto scene_resource_type = repository_workspace.resolve_type(scene_resource_uri, {11, 20}, "DOCK_POPUP");
	expect(scene_resource_type.kind == TypeKind::NativeClass && scene_resource_type.name == "PackedScene",
		"preloaded scene constants resolve as PackedScene resources");

	auto downcast_fixture = std::filesystem::weakly_canonical("tests/fixtures/warnings");
	Workspace downcast_workspace;
	expect(downcast_workspace.open(downcast_fixture, fixture / "extension_api.json", &error),
		"warning workspace opens: " + error);
	auto downcast_uri = downcast_workspace.uri_for_path(downcast_fixture / "main.gd");
	auto downcast_diagnostics = downcast_workspace.diagnostics(downcast_uri);
	expect(diagnostic_count(downcast_diagnostics, "unsafe-call-argument") == 1,
		"unsafe call arguments use the project warning level");
	expect(diagnostic_count(downcast_diagnostics, "unsafe-cast") == 0,
		"broader object assignments and returns remain accepted like Godot");
	auto unsafe_call = std::find_if(downcast_diagnostics.begin(), downcast_diagnostics.end(),
		[](const auto &item) { return item.code == "unsafe-call-argument"; });
	expect(unsafe_call != downcast_diagnostics.end() && unsafe_call->severity == DiagnosticSeverity::Error,
		"unsafe call diagnostic severity follows project.godot");

	if (failures == 0) std::cout << "core tests passed\n";
	return failures == 0 ? 0 : 1;
}
