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

const OutlineSymbol *find_outline(const std::vector<OutlineSymbol> &items, const std::string &name) {
	for (const auto &item : items) {
		if (item.name == name) return &item;
		if (auto *found = find_outline(item.children, name)) return found;
	}
	return nullptr;
}

size_t outline_count(const std::vector<OutlineSymbol> &items, const std::string &name) {
	size_t result = 0;
	for (const auto &item : items) {
		if (item.name == name) ++result;
		result += outline_count(item.children, name);
	}
	return result;
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
		Workspace recovery_workspace;
		auto native_fixture = std::filesystem::weakly_canonical("tests/fixtures/native");
		expect(recovery_workspace.open(native_fixture,
			std::filesystem::weakly_canonical("addons/gdscript_lsp/data/godot-4.6-extension-api.json"), &error),
			"function recovery workspace opens: " + error);
		auto recovery_uri = recovery_workspace.uri_for_path(native_fixture / "function_recovery.gd");
		int64_t version = 1;
		for (bool nested : {false, true}) for (bool inferred : {false, true}) for (bool forward : {false, true}) {
			std::string caller = "func test():\n\tvar n = get_nodes()\n\tn.\n\n";
			std::string factory = inferred ?
				"func get_nodes():\n\tvar owned = Node.new()\n\treturn owned\n\n" :
				"static func get_nodes(\n\tvalue: Node = null,\n\tlabel: String = \"😀\"\n) -> Node:\n"
				"\tvar owned = Node.new()\n\treturn owned\n\n";
			auto functions = (forward ? caller + factory : factory + caller) +
				"func _init(value: int = 1):\n\tvar constructor_local = value\n";
			std::string source = "# 😀 before declarations\n";
			if (nested) source += "class Holder:\n\t";
			for (size_t index = 0; index < functions.size(); ++index) {
				source += functions[index];
				if (nested && functions[index] == '\n' && index + 1 < functions.size()) source += '\t';
			}
			expect(recovery_workspace.update_document(recovery_uri, source, version++, &error), "function blocks update");
			auto position = byte_to_position(source, source.find("n.\n") + 2);
			expect(has_item(recovery_workspace.completion(recovery_uri, position), "queue_free"),
				"Node member completion survives both function orders and class ownership");
			expect(recovery_workspace.resolve_type(recovery_uri, position, "get_nodes()").name == "Node",
				"recovered factory retains declared or inferred return type");
			auto definitions = recovery_workspace.definition(recovery_uri,
				byte_to_position(source, source.find("get_nodes()\n") + 2));
			expect(definitions.size() == 1 && definitions.front().uri == recovery_uri &&
				definitions.front().range.start == byte_to_position(source, source.find("func get_nodes") + 5),
				"forward call definition retains its original source position");
			auto outline = recovery_workspace.document_outline(recovery_uri).symbols;
			auto *test = find_outline(outline, "test");
			auto *get_nodes = find_outline(outline, "get_nodes");
			auto *constructor = find_outline(outline, "_init");
			expect(test && test->return_type && test->return_type->kind == TypeKind::Variant &&
				!find_outline(test->children, "owned") && !find_outline(test->children, "constructor_local"),
				"caller never borrows neighboring returns or locals");
			expect(get_nodes && !get_nodes->malformed && find_outline(get_nodes->children, "owned") &&
				get_nodes->owner_id == (nested ? "res://function_recovery.gd.Holder" : "res://function_recovery.gd"),
				"factory body and signature belong to the correct class");
			if (!inferred && get_nodes) {
				auto *value = find_outline(get_nodes->children, "value");
				auto *label = find_outline(get_nodes->children, "label");
				expect(get_nodes->is_static && value && value->declared_type == "Node" && value->initializer == "null" &&
					label && label->declared_type == "String" && label->initializer == "\"😀\"",
					"multiline recovered signature retains typed defaults and static modifier");
				expect(label && label->range.end == byte_to_position(source, source.find("\"😀\"") + std::string("\"😀\"").size()),
					"recovered parameter ranges retain UTF-16 columns");
			}
			expect(constructor && constructor->kind == SymbolKind::Constructor &&
				find_outline(constructor->children, "constructor_local"), "neighboring constructor retains its identity and locals");
			auto diagnostics = recovery_workspace.diagnostics(recovery_uri);
			expect(has_diagnostic(diagnostics, "syntax-error"), "incomplete member access remains a syntax error");
			expect(std::all_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic &item) {
				return item.code != "syntax-error" || (test && test->range.contains(item.range.start) && test->range.contains(item.range.end));
			}), "recovery diagnostics stay inside the damaged function");
			expect(!has_diagnostic(diagnostics, "undefined-function") && !has_diagnostic(diagnostics, "return-type-mismatch"),
				"semantic analysis uses recovered functions without cross-block errors");
		}
		const std::string invalid_return =
			"func test() -> void:\n\tvar n = get_nodes()\n\tn.\n\nfunc get_nodes() -> Node:\n\treturn 1\n";
		expect(recovery_workspace.update_document(recovery_uri, invalid_return, version++, &error),
			"invalid return following an incomplete function updates");
		auto diagnostics = recovery_workspace.diagnostics(recovery_uri);
		auto *mismatch = find_diagnostic(diagnostics, "return-type-mismatch");
		expect(mismatch && mismatch->range.start == Position{5, 8} && !has_diagnostic(diagnostics, "return-value-in-void"),
			"return diagnostics analyze the later function with its own signature and original position");
	}
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

		const std::string spaced_inference_source =
			"extends RefCounted\n\nvar members_dict: = {}\n\nfunc inspect() -> void:\n"
			"\tvar local_dict:\t= {}\n\tprint(local_dict)\n";
		expect(leading_newline_workspace.update_document(leading_uri, spaced_inference_source, 42, &error),
			"whitespace-separated inference overlay accepted");
		expect(leading_newline_workspace.diagnostics(leading_uri).empty(),
			"whitespace between inferred type tokens produces no diagnostics");
		auto member_dictionary = leading_newline_workspace.resolve_type(leading_uri, {6, 2}, "members_dict");
		auto local_dictionary = leading_newline_workspace.resolve_type(leading_uri, {6, 2}, "local_dict");
		expect(member_dictionary.kind == TypeKind::Builtin && member_dictionary.name == "Dictionary" &&
			local_dictionary.kind == TypeKind::Builtin && local_dictionary.name == "Dictionary",
			"spaced inferred declarations retain member and local Dictionary types");
		auto spaced_completion_source = spaced_inference_source;
		auto print_at = spaced_completion_source.find("\tprint(local_dict)");
		spaced_completion_source.replace(print_at, std::string("\tprint(local_dict)").size(), "\tlocal_dict.");
		expect(leading_newline_workspace.update_document(leading_uri, spaced_completion_source, 43, &error),
			"whitespace-separated inference completion overlay accepted");
		expect(has_item(leading_newline_workspace.completion(leading_uri, {6, 12}), "keys"),
			"spaced inferred Dictionary retains member completion");

		const std::string missing_type_source =
			"extends RefCounted\n\nfunc inspect() -> void:\n"
			"\tvar test_var:\n\t# recovery must stop here\n\tvar n = Missing.Type.VALUE\n\tmissing_after\n";
		expect(leading_newline_workspace.update_document(leading_uri, missing_type_source, 44, &error),
			"incomplete type overlay accepted");
		auto missing_type_diagnostics = leading_newline_workspace.diagnostics(leading_uri);
		expect(std::any_of(missing_type_diagnostics.begin(), missing_type_diagnostics.end(), [](const Diagnostic &item) {
			return item.code == "syntax-error" && item.message == R"(Expected type after ":".)";
		}), "an incomplete type is diagnosed at its declaration boundary");
		expect(diagnostic_count(missing_type_diagnostics, "syntax-error") == 1,
			"an incomplete type produces one quarantined parser diagnostic");
		expect(std::none_of(missing_type_diagnostics.begin(), missing_type_diagnostics.end(), [](const Diagnostic &item) {
			return item.code == "unknown-type" && item.message.find("recovery must stop here") != std::string::npos;
		}), "a recovered type cannot absorb following comments or declarations");
		expect(std::any_of(missing_type_diagnostics.begin(), missing_type_diagnostics.end(), [](const Diagnostic &item) {
			return item.code == "undefined-identifier" && item.message.find("missing_after") != std::string::npos;
		}), "semantic diagnostics continue after a malformed declaration");

		const std::string missing_value_source =
			"extends RefCounted\n\nfunc inspect() -> void:\n\tvar test_var: =\n\tmissing_after\n";
		expect(leading_newline_workspace.update_document(leading_uri, missing_value_source, 45, &error),
			"incomplete initializer overlay accepted");
		auto missing_value_diagnostics = leading_newline_workspace.diagnostics(leading_uri);
		expect(std::any_of(missing_value_diagnostics.begin(), missing_value_diagnostics.end(), [](const Diagnostic &item) {
			return item.code == "syntax-error" && item.message == R"(Expected expression after "=".)";
		}), "an incomplete initializer is diagnosed without borrowing the next line");
		expect(diagnostic_count(missing_value_diagnostics, "syntax-error") == 1,
			"an incomplete initializer produces one quarantined parser diagnostic");

		const std::string multiline_value_source =
			"extends RefCounted\n\nfunc inspect() -> void:\n\tvar values := [\n\t\t1,\n\t]\n";
		expect(leading_newline_workspace.update_document(leading_uri, multiline_value_source, 46, &error),
			"grouped multiline initializer overlay accepted");
		expect(leading_newline_workspace.diagnostics(leading_uri).empty(),
			"balanced multiline initializers remain valid declarations");
	}
	auto completion = workspace.completion(consumer_uri, {6, 7});
	expect(has_item(completion, "own") && !has_item(completion, "new"),
		"instance completion includes direct script members without a constructor");
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
	auto *native_class_item = find_item(unqualified_completion, "RefCounted");
	expect(native_class_item && native_class_item->kind == SymbolKind::Class &&
		native_class_item->label == "RefCounted" && native_class_item->insert_text == "RefCounted" &&
		native_class_item->symbol_id == "native:RefCounted" &&
		native_class_item->origin_id == "native:RefCounted",
		"global completion exposes native API classes with stable identities");
	expect(static_cast<size_t>(std::count_if(completion.begin(), completion.end(), [](const CompletionItem &item) {
		return item.filter_text == "shared_name";
	})) == 1, "derived completion member suppresses the inherited member with the same name");
	expect(completion_ranks_increase(completion), "completion sort ranks preserve server relevance order");

	auto class_completion = workspace.completion(consumer_uri, {9, 14});
	expect(!class_completion.empty() && class_completion.front().filter_text == "new" &&
		has_item(class_completion, "CHILD_CONSTANT") && has_item(class_completion, "child_static") &&
		has_item(class_completion, "BASE_CONSTANT") && has_item(class_completion, "base_static") &&
		!has_item(class_completion, "own") && !has_item(class_completion, "make_base") &&
		!has_item(class_completion, "count") && !has_item(class_completion, "label"),
		"script class receiver offers new first and omits instance members");
	expect(item_index(class_completion, "new") < item_index(class_completion, "CHILD_CONSTANT") &&
		item_index(class_completion, "CHILD_CONSTANT") < item_index(class_completion, "child_static") &&
		item_index(class_completion, "child_static") < item_index(class_completion, "BASE_CONSTANT") &&
		item_index(class_completion, "BASE_CONSTANT") < item_index(class_completion, "base_static"),
		"class receiver preserves source order within each nearest-first inheritance level");

	const std::string alias_completion_source =
		"extends RefCounted\n\n"
		"const ChildAlias = preload(\"res://child.gd\")\n\n"
		"func inspect_type() -> void:\n"
		"\tChildAlias.ch\n"
		"\tRefCounted.re\n";
	expect(workspace.update_document(consumer_uri, alias_completion_source, 1, &error),
		"class-reference completion overlay accepted");
	auto alias_position = byte_to_position(alias_completion_source,
		alias_completion_source.find("ChildAlias.ch") + std::string_view("ChildAlias.ch").size());
	auto alias_class_completion = workspace.completion(consumer_uri, alias_position);
	auto *alias_constructor = find_item(alias_class_completion, "new");
	expect(!alias_class_completion.empty() && alias_class_completion.front().filter_text == "new" && alias_constructor &&
		alias_constructor->label == "new()" && alias_constructor->insert_text == "new()" &&
		has_item(alias_class_completion, "child_static") && has_item(alias_class_completion, "base_static") &&
		!has_item(alias_class_completion, "own") && !has_item(alias_class_completion, "make_base") &&
		!has_item(alias_class_completion, "label"),
		"preloaded script completion starts with new and contains only type-level members");
	auto native_position = byte_to_position(alias_completion_source,
		alias_completion_source.find("RefCounted.re") + std::string_view("RefCounted.re").size());
	auto native_type_completion = workspace.completion(consumer_uri, native_position);
	expect(!native_type_completion.empty() && native_type_completion.front().filter_text == "new" &&
		has_item(native_type_completion, "ref_static") && !has_item(native_type_completion, "reference_method"),
		"native class completion starts with new and excludes instance methods");
	auto alias_type = workspace.resolve_type(consumer_uri, alias_position, "ChildAlias");
	auto alias_instance = workspace.resolve_type(consumer_uri, alias_position, "ChildAlias.new()");
	expect(alias_type.kind == TypeKind::ScriptClass && !alias_type.instance &&
		alias_instance.kind == TypeKind::ScriptClass && alias_instance.instance,
		"preload aliases remain class references until constructed");

	const std::string class_access_source =
		"extends RefCounted\n\n"
		"const ChildAlias = preload(\"res://child.gd\")\n\n"
		"func inspect_type() -> void:\n"
		"\tChildAlias.make_base()\n"
		"\tvar invalid_property = ChildAlias.own\n"
		"\tChildThing.make_base()\n"
		"\tChildAlias.label()\n"
		"\tRefCounted.reference_method()\n"
		"\tChildAlias.missing()\n"
		"\tChildAlias.child_static()\n"
		"\tChildAlias.new().make_base()\n"
		"\tRefCounted.ref_static()\n";
	expect(workspace.update_document(consumer_uri, class_access_source, 2, &error),
		"class-reference diagnostic overlay accepted");
	auto class_access_diagnostics = workspace.diagnostics(consumer_uri);
	expect(diagnostic_count(class_access_diagnostics, "instance-member-access") == 5 &&
		diagnostic_count(class_access_diagnostics, "unknown-member") == 1 &&
		class_access_diagnostics.size() == 6,
		"script aliases, named classes, and native classes reject instance member access without cascades");
	expect(std::any_of(class_access_diagnostics.begin(), class_access_diagnostics.end(), [](const Diagnostic &item) {
		return item.message ==
			"Cannot access instance method \"make_base\" on class \"ChildThing\"; create an instance first.";
	}) && std::any_of(class_access_diagnostics.begin(), class_access_diagnostics.end(), [](const Diagnostic &item) {
		return item.message ==
			"Cannot access instance property \"own\" on class \"ChildThing\"; create an instance first.";
	}), "class-reference diagnostics distinguish methods from properties");
	expect(workspace.close_document(consumer_uri, &error), "class-reference overlay closes");

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
		"reference_method", "native_takes", "_native_virtual", "ref_static", "get_class", "object_static"}),
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
	auto namespace_product = workspace.resolve_expression(inferred_uri, {7, 2}, "Namespace.Product");
	expect(namespace_product.type.kind == TypeKind::ScriptClass && namespace_product.origin &&
		namespace_product.origin->name == "Product",
		"rich resolution exposes the declaration of a recursively resolved inner class");
	expect(!namespace_product.access_paths.empty() && namespace_product.access_paths.front().text == "Namespace.Product" &&
		namespace_product.access_paths.front().kind == AccessPathKind::ScriptAlias,
		"preload access paths rank ahead of other valid spellings");
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
	auto consumer_outline = workspace.document_outline(consumer_uri);
	auto *child_outline = find_outline(consumer_outline.symbols, "child");
	auto *local_outline = find_outline(consumer_outline.symbols, "local");
	expect(child_outline && child_outline->resolved_type.display() == "ChildThing" &&
		child_outline->static_typed && child_outline->inferred &&
		child_outline->detail.find("ChildThing") != std::string::npos,
		"rich document symbols expose true inferred member types");
	expect(local_outline && local_outline->resolved_type.display() == "ChildThing" &&
		local_outline->static_typed && local_outline->is_local,
		"rich document symbols expose typed locals below their function");

	auto alias_uri = workspace.uri_for_path(fixture / "alias_derived.gd");
	expect(workspace.diagnostics(alias_uri).empty(),
		"qualified script aliases provide inherited members, aliases, and enums");
	auto alias_completion = workspace.completion(alias_uri, {5, 1});
	expect(has_item(alias_completion, "inherited_alias_member") && has_item(alias_completion, "Imported") &&
		has_item(alias_completion, "ExitCode"), "completion includes members inherited through a qualified alias");
	auto namespace_uri = workspace.uri_for_path(fixture / "alias_namespace.gd");
	expect(workspace.diagnostics(namespace_uri).empty(),
		"physical inner classes and inner classes extending an outer alias resolve");
	auto namespace_outline = workspace.document_outline(namespace_uri);
	expect(outline_count(namespace_outline.symbols, "PhysicalBase") == 1 &&
		outline_count(namespace_outline.symbols, "LocalDerived") == 1 &&
		std::all_of(namespace_outline.symbols.begin(), namespace_outline.symbols.end(),
			[](const OutlineSymbol &symbol) {
				return std::none_of(symbol.children.begin(), symbol.children.end(),
					[](const OutlineSymbol &child) { return child.kind == SymbolKind::Class; });
			}), "inner classes appear once as separate outline roots");
	auto bridge_uri = workspace.uri_for_path(fixture / "alias_bridge.gd");
	expect(workspace.update_document(bridge_uri, "const BaseAlias = preload(\"res://base.gd\")\n", 3, &error),
		"script alias overlay accepted");
	alias_completion = workspace.completion(alias_uri, {5, 1});
	expect(has_item(alias_completion, "label") && !has_item(alias_completion, "inherited_alias_member"),
		"changing an alias overlay rebuilds dependent inheritance");
	expect(workspace.close_document(bridge_uri, &error), "script alias overlay closes");

	// Compact native counterparts of the asserted access/inference cases in
	// tests_plugin/brohd/gdscript_parser. These exercise inheritance, preload
	// aliases, deep inner classes, name collisions, and declaring-script origin.
	Workspace provenance_workspace;
	auto provenance_fixture = std::filesystem::weakly_canonical("tests/fixtures/provenance");
	expect(provenance_workspace.open(provenance_fixture, fixture / "extension_api.json", &error),
		"provenance fixture opens: " + error);
	auto provenance_uri = provenance_workspace.uri_for_path(provenance_fixture / "main.gd");
	std::ifstream provenance_stream(provenance_fixture / "main.gd");
	std::string provenance_source{std::istreambuf_iterator<char>(provenance_stream), std::istreambuf_iterator<char>()};
	auto provenance_position = [&](std::string_view marker) {
		auto found = provenance_source.find(marker);
		expect(found != std::string::npos, "provenance marker exists: " + std::string(marker));
		return byte_to_position(provenance_source, found + marker.size());
	};
	auto inherited_enum_completion = provenance_workspace.completion_result(provenance_uri,
		provenance_position("if state == "), CompletionProfile::Helpers);
	expect(inherited_enum_completion.disposition == CompletionDisposition::Replace &&
		has_item(inherited_enum_completion.items, "Derived.State.IDLE") &&
		has_item(inherited_enum_completion.items, "ProvenanceBase.State.IDLE") &&
		!has_item(inherited_enum_completion.items, "ProvenanceDerived.State.IDLE"),
		"inherited enum completion exposes the preload alias and declaring global without guessed subclass globals");
	expect(!inherited_enum_completion.items.empty() &&
		inherited_enum_completion.items.front().filter_text == "Derived.State.IDLE" &&
		inherited_enum_completion.items.front().access_kind == "scriptAlias",
		"preload alias enum paths are preferred and retain their access kind");
	auto deep_argument_completion = provenance_workspace.completion_result(provenance_uri,
		provenance_position("object.use_tag("), CompletionProfile::Helpers);
	expect(has_item(deep_argument_completion.items, "Derived.Bundle.Layer.Tag.ONE") &&
		has_item(deep_argument_completion.items, "ProvenanceBase.Bundle.Layer.Tag.ONE"),
		"deep inherited function arguments preserve all usable enum paths");
	auto collision_completion = provenance_workspace.completion_result(provenance_uri,
		provenance_position("inherited_inner.use_mode("), CompletionProfile::Helpers);
	expect(has_item(collision_completion.items, "Derived.Inner.Mode.A") &&
		!has_item(collision_completion.items, "Inner.Mode.A"),
		"access validation rejects a same-named local inner-class decoy");
	auto payload = provenance_workspace.resolve_expression(provenance_uri,
		provenance_position("payload = "), "payload");
	expect(payload.type.kind == TypeKind::ScriptClass && payload.type.name == "Payload" &&
		!payload.access_paths.empty() && payload.access_paths.front().text == "Anonymous.Payload",
		"anonymous-script inner classes are reachable through the caller's preload alias");
	auto deep = provenance_workspace.resolve_expression(provenance_uri,
		provenance_position("deep = "), "deep");
	expect(deep.type.kind == TypeKind::ScriptClass &&
		!deep.access_paths.empty() && deep.access_paths.front().text == "Anonymous.Outer.Deep",
		"deep anonymous inner classes retain their complete access path");
	auto inherited_origin = provenance_workspace.resolve_expression(provenance_uri,
		provenance_position("object.use_tag"), "object.use_tag");
	expect(inherited_origin.origin && inherited_origin.origin->name == "use_tag" &&
		inherited_origin.origin->uri.ends_with("/base.gd"),
		"an inherited callable points to the script that declares it, not the receiver script");

	// End-to-end caret contexts mirror the remaining cases from the exploratory
	// root test.gd without making that user-owned scratch file part of the suite.
	Workspace caret_workspace;
	auto caret_fixture = std::filesystem::weakly_canonical("tests/fixtures/caret_completion");
	expect(caret_workspace.open(caret_fixture, fixture / "extension_api.json", &error),
		"caret completion fixture opens: " + error);
	auto caret_uri = caret_workspace.uri_for_path(caret_fixture / "main.gd");
	const std::string caret_prelude =
		"extends RefCounted\n\n"
		"const TF = ContextRoot.Utils.Profile.TimeFunction.TimeScale\n\n"
		"enum LocalState { IDLE, READY }\n"
		"enum OtherState { FIRST, SECOND }\n\n"
		"func consume(value: ContextRoot.Utils.Profile.TimeFunction.TimeScale) -> void:\n\tpass\n\n"
		"func inspect() -> void:\n";
	int64_t caret_version = 1;
	auto probe = [&](std::string body, std::string_view marker, CompletionProfile profile = CompletionProfile::Full) {
		auto source = caret_prelude + std::move(body);
		auto found = source.rfind(marker);
		expect(found != std::string::npos, "caret completion marker exists");
		auto position = byte_to_position(source, found + marker.size());
		expect(caret_workspace.update_document(caret_uri, std::move(source), ++caret_version, &error),
			"caret completion overlay accepted: " + error);
		return caret_workspace.completion_result(caret_uri, position, profile);
	};
	auto qualified_type = probe("\tvar typed: ContextRoot.Utils.Profile\n", "ContextRoot.Utils.");
	expect(has_item(qualified_type.items, "Profile") && !has_item(qualified_type.items, "helper"),
		"qualified type completion is owned by the type context and excludes functions");
	auto declaration_self = probe("\tvar current := 1\n", "var current");
	expect(!has_item(declaration_self.items, "current"),
		"a local declaration does not suggest the symbol currently being declared");
	auto assignment_self = probe("\tvar current: int\n\tcurrent = current\n", "current = ");
	expect(!has_item(assignment_self.items, "current"),
		"a simple assignment does not suggest its own target on the right-hand side");
	auto logical_enum = probe(
		"\tvar n := OtherState.FIRST\n\tvar em: LocalState\n"
		"\tif em != LocalState.IDLE or n == OtherState.FIRST:\n\t\tpass\n", "n == ");
	expect(logical_enum.disposition == CompletionDisposition::Replace &&
		has_item(logical_enum.items, "OtherState.FIRST"),
		"the nearest logical clause supplies comparison enum completion");
	auto grouped_enum = probe(
		"\tvar em: LocalState\n\tif (em == LocalState.IDLE):\n\t\tpass\n", "em == ");
	expect(has_item(grouped_enum.items, "LocalState.IDLE"),
		"grouping parentheses do not hide comparison enum completion");
	auto logical_enum_at_eof = probe(
		"\tvar n = OtherState.FIRST\n\tvar em: LocalState\n"
		"\tif em != LocalState.IDLE or n == ", "n == ", CompletionProfile::Helpers);
	expect(logical_enum_at_eof.disposition == CompletionDisposition::Replace &&
		has_item(logical_enum_at_eof.items, "OtherState.FIRST"),
		"an incomplete logical comparison at EOF retains its local enum scope");
	auto grouped_enum_at_eof = probe(
		"\tvar em: LocalState\n\tif (em == ", "em == ", CompletionProfile::Helpers);
	expect(grouped_enum_at_eof.disposition == CompletionDisposition::Replace &&
		has_item(grouped_enum_at_eof.items, "LocalState.IDLE"),
		"an incomplete grouped comparison at EOF retains its local enum scope");
	auto blank_match_at_eof = probe(
		"\tvar n = OtherState.FIRST\n\tmatch n:\n\t\t", "match n:\n\t\t", CompletionProfile::Helpers);
	expect(blank_match_at_eof.disposition == CompletionDisposition::Replace &&
		has_item(blank_match_at_eof.items, "OtherState.FIRST"),
		"a blank match pattern resolves the subject enum even when the function is an error node");
	auto partial_match_at_eof = probe(
		"\tvar n = OtherState.FIRST\n\tmatch n:\n\t\tn", "\t\tn", CompletionProfile::Helpers);
	expect(partial_match_at_eof.disposition == CompletionDisposition::Replace &&
		has_item(partial_match_at_eof.items, "OtherState.FIRST"),
		"a partially typed match pattern retains enum-path completion without a colon");
	auto comparison_after_colon = probe(
		"\tvar n = OtherState.FIRST\n\tvar em: LocalState\n"
		"\tif em == LocalState.IDLE and n == OtherState.FIRST:",
		"OtherState.FIRST:", CompletionProfile::Helpers);
	expect(comparison_after_colon.disposition == CompletionDisposition::Replace &&
		comparison_after_colon.provider == "context" && comparison_after_colon.items.empty(),
		"a completed control-flow header suppresses helper completion after its colon");
	auto comparison_after_colon_full = probe(
		"\tvar n = OtherState.FIRST\n\tif n == OtherState.FIRST:   ",
		"OtherState.FIRST:   ", CompletionProfile::Full);
	expect(comparison_after_colon_full.disposition == CompletionDisposition::Replace &&
		comparison_after_colon_full.provider == "context" && comparison_after_colon_full.items.empty(),
		"a structural colon suppresses standalone completion through trailing whitespace");
	auto match_after_colon = probe(
		"\tvar n = OtherState.FIRST\n\tmatch n:\n\t\tOtherState.FIRST:",
		"\t\tOtherState.FIRST:", CompletionProfile::Helpers);
	expect(match_after_colon.disposition == CompletionDisposition::Replace &&
		match_after_colon.provider == "context" && match_after_colon.items.empty(),
		"a completed match pattern suppresses completion after its colon");
	auto recovered_symbols = caret_workspace.document_symbols(caret_uri);
	size_t recovered_inspect_count = 0;
	bool recovered_local = false;
	for (const auto &record : recovered_symbols) for (const auto &member : record.children) {
		if (member.name != "inspect" || member.kind != SymbolKind::Method) continue;
		++recovered_inspect_count;
		recovered_local = std::any_of(member.children.begin(), member.children.end(), [](const Symbol &child) {
			return child.name == "n" && child.is_local;
		});
	}
	expect(recovered_inspect_count == 1 && recovered_local,
		"the outline retains one recovered function and its stable locals during an incomplete match arm");
	auto typed_ternary = probe(
		"\tvar flag := true\n"
		"\tvar state: LocalState = LocalState.IDLE if flag else LocalState.READY\n",
		"else ");
	expect(has_item(typed_ternary.items, "LocalState.IDLE"),
		"an outer declared type supplies completion inside a ternary value branch");
	auto inferred_ternary = probe(
		"\tvar flag := true\n"
		"\tvar state = LocalState.IDLE if flag else LocalState.READY\n",
		"else ");
	expect(has_item(inferred_ternary.items, "LocalState.READY"),
		"an untyped ternary uses its compatible opposite value branch for completion");
	auto constructor_enum = probe(
		"\tContextRoot.Utils.Profile.TimeFunction.new(\"\", TF.USEC)\n", "new(\"\", ");
	expect(has_item(constructor_enum.items, "TF.MSEC") &&
		has_item(constructor_enum.items, "ContextRoot.Utils.Profile.TimeFunction.TimeScale.MSEC") &&
		!has_item(constructor_enum.items, "ContextDecoy.Utils.Profile.TimeFunction.TimeScale.MSEC"),
		"constructor arguments retain declared caller paths without graph-derived global aliases");
	auto constructor_callable = caret_workspace.resolve_expression(caret_uri, {12, 2},
		"ContextRoot.Utils.Profile.TimeFunction.new");
	expect(constructor_callable.origin && constructor_callable.origin->name == "_init" &&
		constructor_callable.origin->uri.ends_with("/time_function.gd"),
		"a synthetic new callable retains the script constructor declaration origin");
	auto resolved_tf = caret_workspace.resolve_expression(caret_uri, {2, 10}, "TF");
	expect(!resolved_tf.access_paths.empty() && resolved_tf.access_paths.front().text == "TF" &&
		std::any_of(resolved_tf.access_paths.begin(), resolved_tf.access_paths.end(), [](const AccessPath &path) {
			return path.text == "ContextRoot.Utils.Profile.TimeFunction.TimeScale";
		}) && std::none_of(resolved_tf.access_paths.begin(), resolved_tf.access_paths.end(), [](const AccessPath &path) {
			return path.text.starts_with("ContextDecoy.");
		}), "rich type resolution returns declared aliases without unrelated global namespace paths");
	auto invalid_constructor_source = caret_prelude +
		"\tContextRoot.Utils.Profile.TimeFunction.new(\"\", \"not a scale\")\n";
	expect(caret_workspace.update_document(caret_uri, invalid_constructor_source, ++caret_version, &error),
		"constructor diagnostic overlay accepted");
	auto constructor_diagnostics = caret_workspace.diagnostics(caret_uri);
	auto invalid_constructor = std::find_if(constructor_diagnostics.begin(), constructor_diagnostics.end(),
		[](const Diagnostic &diagnostic) { return diagnostic.code == "argument-type" &&
			diagnostic.message.find("TimeScale") != std::string::npos &&
			diagnostic.message.find("String") != std::string::npos; });
	expect(invalid_constructor != constructor_diagnostics.end(),
		"foreign constructor parameter types are diagnosed in their declaring class");

	auto unicode = std::string("a😀b\nvalue");
	auto byte = position_to_byte(unicode, {0, 3});
	expect(byte == 5, "UTF-16 position converts surrogate pair");
	expect(byte_to_position(unicode, byte) == Position{0, 3}, "byte converts back to UTF-16 position");
	auto encoded_path = std::filesystem::path("/tmp/gdscript lsp/project.godot");
	auto encoded_uri = file_uri_for_path(encoded_path);
	expect(encoded_uri.find("gdscript%20lsp") != std::string::npos, "file URI percent-encodes spaces");
	expect(path_for_file_uri(encoded_uri) == std::filesystem::absolute(encoded_path).lexically_normal(),
		"file URI round trips to a path");
	expect(canonical_file_uri("file:///c%3a/Users/Dev/My%20Game/main%2Egd").value_or("") ==
#ifdef _WIN32
		"file:///C:/Users/Dev/My%20Game/main.gd",
#else
		"file:///c:/Users/Dev/My%20Game/main.gd",
#endif
		"Windows file URIs normalize drive case and equivalent percent encoding");
	expect(canonical_file_uri("file:///C:/Users/Dev/My%20Game/main.gd").value_or("") ==
		"file:///C:/Users/Dev/My%20Game/main.gd", "canonical Windows file URIs remain stable");
	expect(canonical_file_uri("file://localhost/tmp/project%2Egodot").value_or("") ==
		"file:///tmp/project.godot", "localhost file URIs normalize to the local form");
	expect(!canonical_file_uri("file://remote-host/tmp/project.godot"),
		"remote file URI authorities cannot be canonicalized as local files");
	expect(!canonical_file_uri("file:///tmp/bad%2"), "malformed file URIs cannot be canonicalized");
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
		"\t# comment_probe\n"
		"\tvar string_probe = \"inside\"\n"
		"\tif state == State.IDLE: pass\n"
		"\tvar inferred = State.IDLE\n"
		"\tinferred = State.READY\n"
		"\tprint(target.title)\n"
		"\taccepts({\"state\": state})\n"
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
		helper_position("\taccepts("), CompletionProfile::Helpers);
	expect(enum_argument.disposition == CompletionDisposition::Replace && has_item(enum_argument.items, "State.IDLE"),
		"enum helper resolves a script function argument type");
	auto enum_member_helpers = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("var state: State = State."), CompletionProfile::Helpers);
	expect(enum_member_helpers.disposition == CompletionDisposition::NotHandled && enum_member_helpers.items.empty(),
		"enum helpers leave member access to the active member provider");
	auto enum_member_full = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("var state: State = State."), CompletionProfile::Full);
	expect(enum_member_full.provider == "semantic" &&
		has_item(enum_member_full.items, "IDLE") && has_item(enum_member_full.items, "READY") &&
		!has_item(enum_member_full.items, "State.IDLE"),
		"standalone enum member access returns members rather than expected-value paths");
	auto constructor_result = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("var product: Product = "), CompletionProfile::Helpers);
	auto *constructor = find_item(constructor_result.items, "Product.new");
	expect(constructor_result.disposition == CompletionDisposition::Augment && constructor &&
		constructor->insert_text == "Product.new(" && constructor->origin_id.ends_with("::_init"),
		"constructor helper preserves Godot insertion and points to the _init declaration");
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
	auto parameter_type_hint = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("func accepts(state: "), CompletionProfile::Helpers);
	expect(parameter_type_hint.disposition == CompletionDisposition::Augment &&
		has_item(parameter_type_hint.items, "State"),
		"function parameter annotations are recognized as type contexts");
	auto comparison_enum = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("if state == "), CompletionProfile::Helpers);
	expect(comparison_enum.disposition == CompletionDisposition::Replace &&
		has_item(comparison_enum.items, "State.IDLE"),
		"control-flow comparisons retain the left expression's enum type");
	auto inferred_enum = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("\tinferred = "), CompletionProfile::Helpers);
	expect(inferred_enum.disposition == CompletionDisposition::Replace &&
		has_item(inferred_enum.items, "State.READY"),
		"an enum-valued initializer carries enum identity into later assignments");
	auto comment_completion = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("comment_probe"), CompletionProfile::Full);
	expect(comment_completion.items.empty(), "ordinary completion is suppressed inside comments");
	auto string_completion = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("inside"), CompletionProfile::Full);
	expect(string_completion.items.empty(), "ordinary completion is suppressed inside strings");
	auto resolved_state = diagnostic_workspace.resolve_expression(diagnostic_uri,
		helper_position("if state"), "state");
	expect(resolved_state.type.kind == TypeKind::Enum && resolved_state.origin &&
		resolved_state.origin->name == "state" && resolved_state.origin->symbol_id.find('@') != std::string::npos,
		"rich expression resolution separates an enum type from its unique local declaration origin");
	expect(!resolved_state.access_paths.empty() && resolved_state.access_paths.front().text == "State" &&
		resolved_state.access_paths.front().preferred,
		"rich expression resolution returns a preferred usable access path");
	auto member_result = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("print(target."), CompletionProfile::Full);
	expect(has_item(member_result.items, "title") && !has_item(member_result.items, "_private"),
		"full completion hides private members until an underscore is typed");
	auto dictionary_result = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("{\"state\": "), CompletionProfile::Full);
	expect(dictionary_result.provider == "semantic" && has_item(dictionary_result.items, "state"),
		"a dictionary value outranks an enclosing expected call argument after its colon");
	auto dictionary_helpers = diagnostic_workspace.completion_result(diagnostic_uri,
		helper_position("{\"state\": "), CompletionProfile::Helpers);
	expect(dictionary_helpers.disposition == CompletionDisposition::NotHandled && dictionary_helpers.items.empty(),
		"enum helpers leave a dictionary value inside an enum argument untouched");
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
	const std::string invalid_type_source =
		"extends RefCounted\n\n"
		"const TEXT = \"\"\n"
		"class Holder:\n\tconst VALUE = \"\"\n"
		"var bad: TEXT\n"
		"var nested: Holder.VALUE\n";
	expect(diagnostic_workspace.update_document(diagnostic_uri, invalid_type_source, 101, &error),
		"invalid metatype overlay accepted");
	auto invalid_position = [&](std::string_view marker) {
		auto found = invalid_type_source.find(marker);
		expect(found != std::string::npos, "invalid-type marker exists");
		return byte_to_position(invalid_type_source, found + marker.size());
	};
	auto invalid_type_completion = diagnostic_workspace.completion_result(diagnostic_uri,
		invalid_position("var nested: Holder."), CompletionProfile::Helpers);
	expect(!has_item(invalid_type_completion.items, "VALUE"),
		"qualified value constants are excluded from type-hint completion");
	auto invalid_types = diagnostic_workspace.diagnostics(diagnostic_uri);
	expect(diagnostic_count(invalid_types, "invalid-type") == 2,
		"ordinary local and qualified constants are diagnosed as invalid types");
	auto local_invalid = std::find_if(invalid_types.begin(), invalid_types.end(), [](const Diagnostic &item) {
		return item.code == "invalid-type" && item.message == "\"TEXT\" is a constant but does not contain a type.";
	});
	auto member_invalid = std::find_if(invalid_types.begin(), invalid_types.end(), [](const Diagnostic &item) {
		return item.code == "invalid-type" &&
			item.message == "Member \"VALUE\" under base \"Holder\" is not a valid type.";
	});
	expect(local_invalid != invalid_types.end() && member_invalid != invalid_types.end(),
		"invalid type diagnostics use Godot-compatible messages");
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
		zero_argument_script->insert_text == "nullable_return()" && zero_argument_script->detail == "func",
		"zero-argument script completion uses Godot's compact display and complete call insertion");
	expect(zero_argument_script && !zero_argument_script->symbol_id.empty() &&
		zero_argument_script->origin_id == zero_argument_script->symbol_id &&
		zero_argument_script->provider == "semantic",
		"completion items retain declaration provenance and provider metadata");
	auto resolved_completion = zero_argument_script ?
		diagnostic_workspace.resolve_completion_item(zero_argument_script->symbol_id) : std::nullopt;
	expect(resolved_completion && resolved_completion->detail == "func",
		"completion declarations can be resolved after the initial response");
	auto *argument_script = find_item(script_completion, "accepts_base");
	expect(argument_script && argument_script->label == "accepts_base(\xe2\x80\xa6)" &&
		argument_script->insert_text == "accepts_base(" && argument_script->filter_text == "accepts_base" &&
		argument_script->detail == "func",
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
	expect(has_item(native_class_completion, "ModeFlags") && !has_item(native_class_completion, "READ") &&
		!has_item(native_class_completion, "reference_method"),
		"native type-hint receiver exposes metatypes and omits value and instance members");

	auto warning_fixture = std::filesystem::temp_directory_path() /
		("gdscript-lsp-warning-fixture-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(warning_fixture);
	warning_fixture = std::filesystem::weakly_canonical(warning_fixture);
	auto write_warning_settings = [&](const std::string &settings) {
		std::ofstream stream(warning_fixture / "project.godot");
		stream << settings << "\n[debug]\n"
			"gdscript/warnings/unused_variable=0\n"
			"gdscript/warnings/unused_local_constant=0\n"
			"gdscript/warnings/unused_parameter=0\n"
			"gdscript/warnings/shadowed_variable=0\n"
			"gdscript/warnings/shadowed_variable_base_class=0\n"
			"gdscript/warnings/shadowed_global_identifier=0\n";
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
	auto inference_outline = inference_workspace.document_outline(inference_uri);
	auto *direct_terminal_outline = find_outline(inference_outline.symbols, "direct_terminal");
	auto *seed_outline = find_outline(inference_outline.symbols, "seed_local");
	auto *funk_outline = find_outline(inference_outline.symbols, "funk_test");
	auto *typed_container_outline = find_outline(inference_outline.symbols, "typed_conns");
	auto *signal_arg_outline = find_outline(inference_outline.symbols, "some_arg");
	auto *map_key_outline = find_outline(inference_outline.symbols, "map_key");
	auto *byte_outline = find_outline(inference_outline.symbols, "byte");
	expect(direct_terminal_outline && direct_terminal_outline->resolved_type.display() == "Color" &&
		!direct_terminal_outline->static_typed && direct_terminal_outline->inferred &&
		direct_terminal_outline->origin && direct_terminal_outline->origin->name == "direct_terminal" &&
		direct_terminal_outline->detail.find("(inferred)") != std::string::npos,
		"rich outline labels best-known dynamic local types without claiming a static annotation");
	expect(seed_outline && seed_outline->resolved_type.display() == "Color" && seed_outline->static_typed,
		"rich outline distinguishes true inferred typing from dynamic initializer hints");
	expect(funk_outline && funk_outline->return_type && funk_outline->return_type->display() == "Signal" &&
		funk_outline->detail.find("Signal (inferred)") != std::string::npos,
		"rich outline includes inferred callable return types");
	expect(typed_container_outline && typed_container_outline->resolved_type.display() == "Array[String]" &&
		typed_container_outline->static_typed && !typed_container_outline->inferred,
		"rich outline preserves explicit typed-container details");
	expect(signal_arg_outline && signal_arg_outline->resolved_type.display() == "int" &&
		signal_arg_outline->is_parameter && signal_arg_outline->static_typed,
		"rich outline resolves nested signal parameters");
	expect(map_key_outline && map_key_outline->resolved_type.display() == "InferenceEnum" &&
		!map_key_outline->static_typed && map_key_outline->inferred &&
		map_key_outline->detail.starts_with("for map_key:"),
		"rich outline exposes inferred typed-dictionary iteration variables");
	expect(byte_outline && byte_outline->resolved_type.display() == "int" &&
		!byte_outline->static_typed && byte_outline->inferred && byte_outline->detail.starts_with("for byte:"),
		"rich outline exposes inferred packed-array iteration variables");
	std::ifstream changed_inference_stream(inference_fixture / "scenario.gd", std::ios::binary);
	std::string changed_inference_source{std::istreambuf_iterator<char>(changed_inference_stream),
		std::istreambuf_iterator<char>()};
	auto terminal_assignment = changed_inference_source.find("var direct_terminal = seed_local");
	expect(terminal_assignment != std::string::npos, "outline cache invalidation fixture assignment exists");
	if (terminal_assignment != std::string::npos) {
		changed_inference_source.replace(terminal_assignment, std::string("var direct_terminal = seed_local").size(),
			"var direct_terminal = 42");
	}
	UpdateImpact outline_impact;
	expect(inference_workspace.update_document(inference_uri, changed_inference_source, 12, &error, &outline_impact),
		"outline cache body edit updates");
	auto changed_outline = inference_workspace.document_outline(inference_uri);
	direct_terminal_outline = find_outline(changed_outline.symbols, "direct_terminal");
	expect(!outline_impact.full_rebuild && direct_terminal_outline &&
		direct_terminal_outline->resolved_type.display() == "int",
		"body edits invalidate the cached outline and refresh inferred types");
	expect(inference_workspace.close_document(inference_uri, &error), "outline inference overlay closes");
	auto restored_outline = inference_workspace.document_outline(inference_uri);
	direct_terminal_outline = find_outline(restored_outline.symbols, "direct_terminal");
	expect(direct_terminal_outline && direct_terminal_outline->resolved_type.display() == "Color",
		"closing an overlay invalidates the outline cache and restores disk types");
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
	expect(position_type.kind == TypeKind::Enum && position_type.name == "Position",
		"qualified nested enum values retain their enum identity");
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
