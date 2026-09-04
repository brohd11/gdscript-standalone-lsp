#include "core/text.hpp"
#include "core/workspace.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace gdscript_lsp;

namespace {

int failures = 0;

void expect(bool condition, std::string message) {
	if (condition) return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}

void expect_result(bool condition, std::string message, const CompletionResult &result) {
	if (condition) return;
	std::cerr << "FAIL: " << message << " [provider=" << result.provider << ", disposition="
		<< static_cast<int>(result.disposition) << ", items=";
	for (const auto &item : result.items) {
		std::cerr << item.filter_text << "{label=" << item.label << ",insert=" << item.insert_text
			<< ",origin=" << item.origin_id << "},";
	}
	std::cerr << "]\n";
	++failures;
}

const CompletionItem *find_item(const CompletionResult &result, std::string_view filter_text) {
	for (const auto &item : result.items) if (item.filter_text == filter_text) return &item;
	return nullptr;
}

bool has_item(const CompletionResult &result, std::string_view filter_text) {
	return find_item(result, filter_text) != nullptr;
}

const std::string prelude =
	"extends CompletionProviderBase\n\n"
	"enum State { IDLE, READY }\n"
	"const ProductAlias = Product\n\n"
	"class ProductZero:\n"
	"\tfunc _init() -> void: pass\n\n"
	"class Product extends CompletionProviderBase:\n"
	"\tclass Nested: pass\n"
	"\tvar title: String\n"
	"\tvar child: Product\n"
	"\tvar _private_property: int\n"
	"\tfunc _init(required: int) -> void: pass\n"
	"\tfunc build(value: int = 0) -> void: pass\n"
	"\tfunc _private_method() -> void: pass\n\n"
	"func accepts_state(index: int, state: State) -> void: pass\n"
	"func accepts_product(value: Product) -> void: pass\n\n";

struct Harness {
	Workspace workspace;
	std::string uri;
	int64_t version = 1;
	std::string error;

	bool open() {
		auto fixture = std::filesystem::weakly_canonical("tests/fixtures/completion_providers");
		auto api = std::filesystem::weakly_canonical("tests/fixtures/basic/extension_api.json");
		if (!workspace.open(fixture, api, &error)) return false;
		uri = workspace.uri_for_path(fixture / "main.gd");
		return true;
	}

	CompletionResult probe(std::string source, CompletionProfile profile = CompletionProfile::Helpers) {
		constexpr std::string_view marker = "<caret>";
		auto offset = source.find(marker);
		expect(offset != std::string::npos, "probe contains a caret marker");
		expect(offset == std::string::npos || source.find(marker, offset + marker.size()) == std::string::npos,
			"probe contains exactly one caret marker");
		if (offset == std::string::npos) return {};
		source.erase(offset, marker.size());
		auto position = byte_to_position(source, offset);
		expect(workspace.update_document(uri, std::move(source), ++version, &error),
			"completion overlay updates: " + error);
		return workspace.completion_result(uri, position, profile);
	}

	CompletionResult body(std::string body, CompletionProfile profile = CompletionProfile::Helpers) {
		return probe(prelude + "func inspect(target: Product, base: CompletionProviderBase, map: Dictionary, flag: bool) -> void:\n" +
			std::move(body), profile);
	}

	void reset_config() { workspace.set_completion_config(CompletionConfig{}); }
};

void check_enum_provider(Harness &harness) {
	auto assignment = harness.body("\tvar state: State = <caret>\n");
	expect(assignment.disposition == CompletionDisposition::Replace && assignment.provider == "enums" &&
		has_item(assignment, "State.IDLE") && has_item(assignment, "State.READY"),
		"script enum owns a typed assignment");

	auto reassignment = harness.body("\tvar state: State = State.IDLE\n\tstate = <caret>\n");
	expect(has_item(reassignment, "State.READY"), "typed enum reassignment retains enum identity");
	auto inferred = harness.body("\tvar state = State.IDLE\n\tstate = <caret>\n");
	expect(has_item(inferred, "State.READY"), "inferred enum reassignment retains enum identity");

	for (auto operation : {"==", "!=", "<", "<=", ">", ">="}) {
		auto comparison = harness.body("\tvar state: State\n\tif state " + std::string(operation) + " <caret>\n");
		expect(has_item(comparison, "State.IDLE"), "enum comparison supports operator " + std::string(operation));
	}

	auto argument = harness.body("\taccepts_state(1, <caret>)\n");
	expect(has_item(argument, "State.IDLE"), "enum completion uses a non-first callable argument");
	auto conditional = harness.body(
		"\tvar state = State.IDLE if flag else <caret>\n");
	expect_result(has_item(conditional, "State.READY"), "enum completion uses the opposite conditional value branch", conditional);
	auto match_pattern = harness.body(
		"\tvar state: State\n\tmatch state:\n\t\t<caret>\n");
	expect(has_item(match_pattern, "State.IDLE"), "enum completion uses a match subject");

	auto global_enum = harness.body("\tvar error: Error = <caret>\n");
	expect(has_item(global_enum, "OK") && !has_item(global_enum, "Error.OK"),
		"global enums use their global constant spelling");
	auto native_enum = harness.body("\tvar mode: FileAccess.ModeFlags = <caret>\n");
	expect(has_item(native_enum, "FileAccess.READ") && has_item(native_enum, "FileAccess.WRITE"),
		"native class enums use their class-qualified spelling");

	auto member = harness.body("\tvar state: State = State.<caret>\n");
	expect(member.disposition == CompletionDisposition::NotHandled && member.items.empty(),
		"enum helper does not own member access");
	auto dictionary = harness.body("\taccepts_state(1, {\"state\": <caret>})\n");
	expect(dictionary.disposition == CompletionDisposition::NotHandled && dictionary.items.empty(),
		"enum helper does not leak into a dictionary inside its argument");
	auto array = harness.body("\taccepts_state(1, [<caret>])\n");
	expect(array.disposition == CompletionDisposition::NotHandled && array.items.empty(),
		"enum helper does not leak into an array inside its argument");

	auto config = harness.workspace.completion_config();
	config.enums = false;
	harness.workspace.set_completion_config(config);
	auto disabled = harness.body("\tvar state: State = <caret>\n");
	expect(disabled.disposition == CompletionDisposition::NotHandled && disabled.items.empty(),
		"enum setting disables the enum provider");
	harness.reset_config();
}

void check_extended_types(Harness &harness) {
	struct TypeCase { const char *name; std::string source; std::string expected; };
	std::vector<TypeCase> cases = {
		{"variable", prelude + "var value: <caret>\n", "Product"},
		{"parameter", prelude + "func use(value: <caret>) -> void: pass\n", "Product"},
		{"return", prelude + "func make() -> <caret>\n", "Product"},
		{"extends", "extends <caret>\n", "CompletionProviderBase"},
		{"as", prelude + "func inspect(value):\n\tvar cast = value as <caret>\n", "Product"},
		{"is", prelude + "func inspect(value):\n\tif value is <caret>\n", "Product"},
		{"is not", prelude + "func inspect(value):\n\tif value is not <caret>\n", "Product"},
		{"typed container", prelude + "var values: Array[<caret>]\n", "Product"},
	};
	for (auto &test : cases) {
		auto result = harness.probe(std::move(test.source));
		expect_result(result.disposition == CompletionDisposition::Augment && result.provider == "extendedTypeHints" &&
			has_item(result, test.expected), "extended type completion handles " + std::string(test.name), result);
	}
	auto native = harness.probe(prelude + "var reference: <caret>\n");
	expect_result(has_item(native, "RefCounted"),
		"native API classes remain available in type-hint completion", native);

	auto qualified = harness.probe(prelude + "var value: Product.<caret>\n");
	expect(has_item(qualified, "Nested") && !has_item(qualified, "title"),
		"qualified type completion includes metatypes and excludes values");
	auto native_qualified = harness.probe(prelude + "var value: FileAccess.<caret>\n");
	expect(has_item(native_qualified, "ModeFlags") && !has_item(native_qualified, "READ"),
		"qualified native type completion exposes enums but not enum values");

	auto config = harness.workspace.completion_config();
	config.extended_type_hints = false;
	harness.workspace.set_completion_config(config);
	auto disabled = harness.probe(prelude + "var value: <caret>\n");
	expect(disabled.disposition == CompletionDisposition::NotHandled && disabled.items.empty(),
		"extended type setting disables the provider");
	harness.reset_config();
}

void check_constructors(Harness &harness) {
	auto parameterized = harness.body("\tvar product: Product = <caret>\n");
	auto *product = find_item(parameterized, "ProductAlias.new");
	expect_result(parameterized.disposition == CompletionDisposition::Augment && parameterized.provider == "constructors" && product,
		"parameterized script constructor retains insertion and origin", parameterized);
	if (product) {
		expect(product->label.starts_with("ProductAlias.new(") && product->label.ends_with(')') &&
			product->label != "ProductAlias.new()",
			"parameterized constructor uses the compact ellipsis label");
		expect(product->insert_text == "ProductAlias.new(",
			"parameterized constructor inserts an opening parenthesis");
		expect(product->origin_id.ends_with("::_init"),
			"parameterized constructor points to its _init declaration");
	}
	auto zero = harness.body("\tvar product: ProductZero = <caret>\n");
	auto *zero_item = find_item(zero, "ProductZero.new");
	expect(zero_item && zero_item->label == "ProductZero.new()" && zero_item->insert_text == "ProductZero.new()",
		"zero-argument script constructor inserts a complete call");
	auto native = harness.body("\tvar reference: RefCounted = <caret>\n");
	expect(has_item(native, "RefCounted.new"), "native class expected types offer constructors");
	auto alias = harness.body("\tvar product: ProductAlias = <caret>\n");
	expect(has_item(alias, "ProductAlias.new") || has_item(alias, "Product.new"),
		"script aliases retain a usable constructor path");
	auto argument = harness.body("\taccepts_product(<caret>)\n");
	expect_result(has_item(argument, "ProductAlias.new"), "script constructor is offered for a callable argument", argument);
	auto comparison = harness.body("\tif target == <caret>\n");
	expect_result(has_item(comparison, "ProductAlias.new"), "script constructor is offered for an object comparison", comparison);
	auto member = harness.body("\tvar product: Product = Product.<caret>\n");
	expect(member.disposition == CompletionDisposition::NotHandled && member.items.empty(),
		"constructor helper does not leak into member access");

	auto config = harness.workspace.completion_config();
	config.constructors = false;
	harness.workspace.set_completion_config(config);
	auto disabled = harness.body("\tvar product: Product = <caret>\n");
	expect(disabled.disposition == CompletionDisposition::NotHandled && disabled.items.empty(),
		"constructor setting disables the provider");
	harness.reset_config();
}

void check_member_strings(Harness &harness) {
	for (auto call : {"call", "call_deferred", "callv", "call_thread_safe",
			"call_deferred_thread_group", "has_method", "rpc"}) {
		auto result = harness.body("\ttarget." + std::string(call) + "(\"<caret>\")\n");
		expect(result.disposition == CompletionDisposition::Replace && result.provider == "memberStrings" &&
			has_item(result, "build") && !has_item(result, "title"),
			"member-string method dispatch handles " + std::string(call));
	}
	auto rpc_id = harness.body("\ttarget.rpc_id(1, \"<caret>\")\n");
	expect(has_item(rpc_id, "build"), "rpc_id uses its second argument for method completion");
	auto callable = harness.body("\tCallable(target, \"<caret>\")\n");
	expect(has_item(callable, "build"), "Callable uses its first argument as the receiver");

	for (auto call : {"get", "set", "set_deferred"}) {
		auto result = harness.body("\ttarget." + std::string(call) + "(\"<caret>\")\n");
		expect(has_item(result, "title") && !has_item(result, "build"),
			"member-string property dispatch handles " + std::string(call));
	}
	auto indexed = harness.body("\ttarget.get_indexed(\"child:<caret>\")\n");
	expect(has_item(indexed, "child:title"), "indexed property completion walks a nested property path");
	auto set_indexed = harness.body("\ttarget.set_indexed(\"child:<caret>\", null)\n");
	expect(has_item(set_indexed, "child:title"), "set_indexed uses its first argument and nested receiver");
	auto tween = harness.body("\ttween_property(target, \"child:<caret>\", null, 1.0)\n");
	expect(has_item(tween, "child:title"), "tween_property uses its first argument as the receiver");

	auto native = harness.body("\tbase.call(\"<caret>\")\n");
	expect(has_item(native, "inherited_method") && has_item(native, "reference_method"),
		"member-string completion includes script and native inherited methods");
	auto quoted = harness.body("\ttarget.call(<caret>)\n");
	auto *quoted_build = find_item(quoted, "build");
	expect(quoted_build && quoted_build->insert_text == "&\"build\"",
		"member-string completion inserts a StringName outside an existing string");
	auto in_string = harness.body("\ttarget.call(\"<caret>\")\n");
	auto *raw_build = find_item(in_string, "build");
	expect(raw_build && raw_build->insert_text == "build",
		"member-string completion inserts raw text inside an existing string");

	auto config = harness.workspace.completion_config();
	config.member_strings_prefer_string_name = false;
	harness.workspace.set_completion_config(config);
	auto plain_string = harness.body("\ttarget.call(<caret>)\n");
	auto *plain_build = find_item(plain_string, "build");
	expect(plain_build && plain_build->insert_text == "\"build\"",
		"member-string quoting respects preferStringName");
	auto node_path = harness.body("\ttarget.get_indexed(<caret>)\n");
	auto *node_child = find_item(node_path, "child");
	expect(node_child && node_child->insert_text == "^\"child\"",
		"indexed property completion inserts a NodePath outside an existing string");

	config = harness.workspace.completion_config();
	config.member_strings_include_private = true;
	harness.workspace.set_completion_config(config);
	auto private_result = harness.body("\ttarget.call(\"<caret>\")\n");
	expect(has_item(private_result, "_private_method"), "member-string completion can include private methods");

	config = harness.workspace.completion_config();
	config.member_strings = false;
	harness.workspace.set_completion_config(config);
	auto disabled = harness.body("\ttarget.call(\"<caret>\")\n");
	expect(disabled.disposition == CompletionDisposition::NotHandled && disabled.items.empty(),
		"disabled member strings fall through in helpers mode");
	harness.reset_config();

	auto wrong_argument = harness.body("\ttarget.call(\"build\", \"<caret>\")\n");
	expect(wrong_argument.disposition == CompletionDisposition::NotHandled && wrong_argument.items.empty(),
		"member strings do not own the wrong argument position");
	auto dictionary = harness.body("\tmap.call(\"<caret>\")\n");
	expect(dictionary.disposition == CompletionDisposition::NotHandled && dictionary.items.empty(),
		"member strings do not replace Dictionary's dynamic call target");
	auto unresolved = harness.body("\tunknown.call(\"<caret>\")\n");
	expect(unresolved.disposition == CompletionDisposition::NotHandled && unresolved.items.empty(),
		"member strings fall through for unresolved receivers");
}

void check_private_filter(Harness &harness) {
	auto hidden = harness.body("\ttarget.<caret>\n", CompletionProfile::Full);
	expect(has_item(hidden, "title") && has_item(hidden, "inherited_property") &&
		!has_item(hidden, "_private_property") && !has_item(hidden, "_private_method") &&
		!has_item(hidden, "_inherited_private") && !has_item(hidden, "_inherited_method"),
		"private filtering covers local and inherited members");
	auto requested = harness.body("\ttarget._<caret>\n", CompletionProfile::Full);
	expect(has_item(requested, "_private_property") && has_item(requested, "_inherited_private"),
		"an underscore prefix reveals private members");

	auto config = harness.workspace.completion_config();
	config.hide_private = false;
	harness.workspace.set_completion_config(config);
	auto disabled = harness.body("\ttarget.<caret>\n", CompletionProfile::Full);
	expect(has_item(disabled, "_private_property") && has_item(disabled, "_inherited_private"),
		"hidePrivate setting disables private filtering");
	harness.reset_config();
}

} // namespace

int main() {
	Harness harness;
	if (!harness.open()) {
		std::cerr << "FAIL: completion provider fixture opens: " << harness.error << '\n';
		return 1;
	}
	check_enum_provider(harness);
	check_extended_types(harness);
	check_constructors(harness);
	check_member_strings(harness);
	check_private_filter(harness);
	if (failures) {
		std::cerr << failures << " completion-provider test(s) failed\n";
		return 1;
	}
	std::cout << "completion provider tests passed\n";
	return 0;
}
