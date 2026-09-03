#include "core/caret_context.hpp"
#include "core/text.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gdscript_lsp;

namespace {

int failures = 0;

struct Expected {
	std::string name;
	std::string source;
	CaretLexicalContext lexical = CaretLexicalContext::Code;
	std::optional<CaretRole> role;
	std::optional<bool> member_access;
	std::optional<std::string> receiver;
	std::optional<std::string> prefix;
	std::optional<std::string> callee;
	std::optional<size_t> argument;
	std::optional<std::string> operation;
	std::optional<std::string> left;
	std::optional<bool> type_hint;
	std::optional<std::string> suppressed;
	std::optional<ConditionalBranch> branch;
	Expected(std::string p_name, std::string p_source,
			CaretLexicalContext p_lexical = CaretLexicalContext::Code,
			std::optional<CaretRole> p_role = {}, std::optional<bool> p_member_access = {},
			std::optional<std::string> p_receiver = {}, std::optional<std::string> p_prefix = {},
			std::optional<std::string> p_callee = {}, std::optional<size_t> p_argument = {},
			std::optional<std::string> p_operation = {}, std::optional<std::string> p_left = {},
			std::optional<bool> p_type_hint = {}, std::optional<std::string> p_suppressed = {},
			std::optional<ConditionalBranch> p_branch = {}) :
			name(std::move(p_name)), source(std::move(p_source)), lexical(p_lexical), role(p_role),
			member_access(p_member_access), receiver(std::move(p_receiver)), prefix(std::move(p_prefix)),
			callee(std::move(p_callee)), argument(p_argument), operation(std::move(p_operation)),
			left(std::move(p_left)), type_hint(p_type_hint), suppressed(std::move(p_suppressed)), branch(p_branch) {}
};

void fail(const Expected &expected, std::string_view field, std::string actual) {
	std::cerr << "FAIL [" << expected.name << "] " << field << ": " << actual << '\n'
		<< expected.source << '\n';
	++failures;
}

void check(Expected expected) {
	constexpr std::string_view marker = "<caret>";
	auto offset = expected.source.find(marker);
	if (offset == std::string::npos || expected.source.find(marker, offset + marker.size()) != std::string::npos) {
		fail(expected, "fixture", "expected exactly one <caret>");
		return;
	}
	expected.source.erase(offset, marker.size());
	Document document("file:///caret_context.gd", "res://caret_context.gd", expected.source, 1);
	auto context = analyze_caret(document, byte_to_position(expected.source, offset));
	if (context.lexical != expected.lexical) fail(expected, "lexical", std::to_string(static_cast<int>(context.lexical)));
	if (expected.role && context.role != *expected.role) fail(expected, "role", std::to_string(static_cast<int>(context.role)));
	if (expected.member_access && context.member_access != *expected.member_access) {
		fail(expected, "member_access", context.member_access ? "true" : "false");
	}
	if (expected.receiver && context.member_receiver.value_or("<none>") != *expected.receiver) {
		fail(expected, "receiver", context.member_receiver.value_or("<none>"));
	}
	if (expected.prefix && context.member_prefix != *expected.prefix) fail(expected, "prefix", context.member_prefix);
	if (expected.callee && (!context.call || context.call->callee != *expected.callee)) {
		fail(expected, "callee", context.call ? context.call->callee : "<none>");
	}
	if (expected.argument && (!context.call || context.call->argument_index != *expected.argument)) {
		fail(expected, "argument", context.call ? std::to_string(context.call->argument_index) : "<none>");
	}
	if (expected.operation && (!context.operation || context.operation->operation != *expected.operation)) {
		fail(expected, "operation", context.operation ? context.operation->operation : "<none>");
	}
	if (expected.left && (!context.operation || context.operation->left_expression != *expected.left)) {
		fail(expected, "left", context.operation ? context.operation->left_expression : "<none>");
	}
	if (expected.type_hint && context.in_type_hint != *expected.type_hint) {
		fail(expected, "type_hint", context.in_type_hint ? "true" : "false");
	}
	if (expected.suppressed && context.suppressed_symbol != *expected.suppressed) {
		fail(expected, "suppressed", context.suppressed_symbol);
	}
	if (expected.branch && (!context.conditional || context.conditional->branch != *expected.branch)) {
		fail(expected, "conditional", context.conditional ?
			std::to_string(static_cast<int>(context.conditional->branch)) : "<none>");
	}
}

} // namespace

int main() {
	std::vector<Expected> cases = {
		{"comment", "func f():\n\tvalue # comment <caret>", CaretLexicalContext::Comment},
		{"hash in string", "func f():\n\tvar x = \"# still <caret>string\"", CaretLexicalContext::String},
		{"escaped quote", "func f():\n\tvar x = \"escaped \\\" <caret>text\"", CaretLexicalContext::String},
		{"triple string", "func f():\n\tvar x = \"\"\"multi\n<caret>line\"\"\"", CaretLexicalContext::String},
		{"string name", "func f():\n\tvar x = &\"mem<caret>ber\"", CaretLexicalContext::StringName},
		{"node path", "func f():\n\tvar x = ^\"Node/<caret>Child\"", CaretLexicalContext::NodePath},

		{"simple member", "func f():\n\tobject.mem<caret>", CaretLexicalContext::Code,
			CaretRole::MemberAccess, true, "object", "mem"},
		{"recursive member", "func f():\n\tobject.make(\"a.b\")[0].mem<caret>", CaretLexicalContext::Code,
			CaretRole::MemberAccess, true, "object.make(\"a.b\")[0]", "mem"},
		{"malformed member", "func f():\n\tobject(.mem<caret>", CaretLexicalContext::Code,
			CaretRole::MemberAccess, true},

		{"first call argument", "func f():\n\touter(<caret>)", CaretLexicalContext::Code,
			CaretRole::CallArgument, {}, {}, {}, "outer", 0},
		{"nested call", "func f():\n\touter([1, 2], inner(\"a,b\", <caret>))", CaretLexicalContext::Code,
			CaretRole::CallArgument, {}, {}, {}, "inner", 1},
		{"multiline call", "func f():\n\touter(1,\n\t\t{\"items\": [1, 2]},\n\t\t<caret>)", CaretLexicalContext::Code,
			CaretRole::CallArgument, {}, {}, {}, "outer", 2},
		{"grouping is not call", "func f(value):\n\tif (value == <caret>): pass", CaretLexicalContext::Code,
			CaretRole::ComparisonRight, {}, {}, {}, {}, {}, "==", "value"},

		{"logical comparison", "func f(em, n):\n\tif em != 1 or n == <caret>: pass", CaretLexicalContext::Code,
			CaretRole::ComparisonRight, {}, {}, {}, {}, {}, "==", "n"},
		{"nested comparison", "func f(em):\n\tif ((em == <caret>)): pass", CaretLexicalContext::Code,
			CaretRole::ComparisonRight, {}, {}, {}, {}, {}, "==", "em"},
		{"logical comparison at eof", "func f(em, n):\n\tif em != 1 or n == <caret>", CaretLexicalContext::Code,
			CaretRole::ComparisonRight, {}, {}, {}, {}, {}, "==", "n"},
		{"grouped comparison at eof", "func f(em):\n\tif (em == <caret>", CaretLexicalContext::Code,
			CaretRole::ComparisonRight, {}, {}, {}, {}, {}, "==", "em"},
		{"operator in string", "func f(n):\n\tif n == \"not != an op\" or n >= <caret>: pass", CaretLexicalContext::Code,
			CaretRole::ComparisonRight, {}, {}, {}, {}, {}, ">=", "n"},
		{"arithmetic operation", "func f(n):\n\tvar x = n * <caret>", CaretLexicalContext::Code,
			{}, {}, {}, {}, {}, {}, "*", "n"},

		{"variable type", "var value: Some.Type<caret>", CaretLexicalContext::Code,
			CaretRole::TypeHint, {}, {}, {}, {}, {}, {}, {}, true},
		{"parameter type", "func f(value: Array[String<caret>]): pass", CaretLexicalContext::Code,
			CaretRole::TypeHint, {}, {}, {}, {}, {}, {}, {}, true},
		{"return type", "func f() -> Dictionary[String, int<caret>]: pass", CaretLexicalContext::Code,
			CaretRole::TypeHint, {}, {}, {}, {}, {}, {}, {}, true},
		{"as type", "func f(value):\n\tvar cast = value as Some.Type<caret>", CaretLexicalContext::Code,
			CaretRole::TypeHint, {}, {}, {}, {}, {}, {}, {}, true},
		{"dictionary value is not type", "func f():\n\tvar map = {\"key\": <caret>}", CaretLexicalContext::Code,
			CaretRole::DictionaryEntry, {}, {}, {}, {}, {}, {}, {}, false},

		{"array element", "func f():\n\tvar values = [1, <caret>]", CaretLexicalContext::Code, CaretRole::ArrayElement},
		{"subscript", "func f(values, index):\n\tvar value = values[index + <caret>]", CaretLexicalContext::Code,
			CaretRole::IndexAccess, {}, {}, {}, {}, {}, "+", "index"},
		{"declaration self", "func f():\n\tvar current<caret>", CaretLexicalContext::Code,
			{}, {}, {}, {}, {}, {}, {}, {}, {}, "current"},
		{"assignment self", "func f():\n\tcurrent = nested(<caret>)", CaretLexicalContext::Code,
			CaretRole::CallArgument, {}, {}, {}, "nested", 0, {}, {}, {}, "current"},

		{"ternary condition", "func f(flag):\n\tvar x = 1 if flag <caret>else 2", CaretLexicalContext::Code,
			CaretRole::ConditionalCondition, {}, {}, {}, {}, {}, {}, {}, {}, {}, ConditionalBranch::Condition},
		{"ternary false", "func f(flag):\n\tvar x = 1 if flag else <caret>", CaretLexicalContext::Code,
			CaretRole::ConditionalFalse, {}, {}, {}, {}, {}, {}, {}, {}, {}, ConditionalBranch::FalseValue},
		{"ternary true", "func f(flag):\n\tvar x = tru<caret>e if flag else false", CaretLexicalContext::Code,
			CaretRole::ConditionalTrue, {}, {}, {}, {}, {}, {}, {}, {}, {}, ConditionalBranch::TrueValue},
		{"nested ternary", "func f(a, b):\n\tvar x = (1 if a else 2) if b else <caret>", CaretLexicalContext::Code,
			CaretRole::ConditionalFalse, {}, {}, {}, {}, {}, {}, {}, {}, {}, ConditionalBranch::FalseValue},

		{"blank match pattern", "func f(n):\n\tmatch n:\n\t\t<caret>", CaretLexicalContext::Code,
			CaretRole::MatchPattern},
		{"partial match pattern", "func f(n):\n\tmatch n:\n\t\tn<caret>", CaretLexicalContext::Code,
			CaretRole::MatchPattern},
		{"match arm body", "func f(n):\n\tmatch n:\n\t\t_:\n\t\t\tvalue<caret>", CaretLexicalContext::Code,
			CaretRole::None},
	};
	for (auto &test : cases) check(std::move(test));
	if (failures) {
		std::cerr << failures << " caret-context test(s) failed\n";
		return 1;
	}
	std::cout << "caret context tests passed\n";
	return 0;
}
