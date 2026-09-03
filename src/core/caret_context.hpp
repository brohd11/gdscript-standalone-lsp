#pragma once

#include "core/document.hpp"

#include <optional>
#include <string>
#include <vector>

namespace gdscript_lsp {

enum class CaretLexicalContext : uint8_t {
	Code,
	Comment,
	String,
	StringName,
	NodePath,
};

enum class CaretRole : uint8_t {
	None,
	Suppressed,
	MemberAccess,
	TypeHint,
	AssignmentValue,
	ComparisonRight,
	CallArgument,
	MatchPattern,
	IndexAccess,
	ArrayElement,
	DictionaryEntry,
	ConditionalTrue,
	ConditionalCondition,
	ConditionalFalse,
};

enum class ConditionalBranch : uint8_t {
	None,
	TrueValue,
	Condition,
	FalseValue,
};

struct CaretCallContext {
	std::string callee;
	std::vector<std::string> arguments;
	size_t argument_index = 0;
	bool in_string = false;
	char quote = 0;
};

struct CaretOperationContext {
	std::string operation;
	std::string left_expression;
	std::string right_expression;
};

struct CaretConditionalContext {
	ConditionalBranch branch = ConditionalBranch::None;
	std::string true_expression;
	std::string condition_expression;
	std::string false_expression;
};

// A parser-independent description of the expression surrounding an LSP
// caret. It is deliberately a value type so the same result can later be
// wrapped by GDExtension without exposing tree-sitter objects.
struct CaretContext {
	CaretLexicalContext lexical = CaretLexicalContext::Code;
	CaretRole role = CaretRole::None;
	size_t byte_offset = 0;
	size_t statement_start = 0;
	bool member_access = false;
	std::optional<std::string> member_receiver;
	std::string member_prefix;
	std::optional<CaretCallContext> call;
	std::optional<CaretOperationContext> operation;
	std::optional<CaretConditionalContext> conditional;
	std::string assignment_left;
	std::string assignment_right;
	std::string declared_type;
	std::string suppressed_symbol;
	std::string match_expression;
	bool in_type_hint = false;
};

CaretContext analyze_caret(const Document &document, Position position);

} // namespace gdscript_lsp
