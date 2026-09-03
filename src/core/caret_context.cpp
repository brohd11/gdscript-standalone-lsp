#include "core/caret_context.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_set>

namespace gdscript_lsp {
namespace {

struct Delimiter {
	char value = 0;
	size_t offset = 0;
	std::vector<size_t> commas;
};

struct ScanResult {
	std::vector<bool> code;
	std::vector<Delimiter> stack;
	size_t statement_start = 0;
	CaretLexicalContext lexical = CaretLexicalContext::Code;
	char quote = 0;
};

bool identifier_character(char value) {
	return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

bool identifier_start(char value) {
	return std::isalpha(static_cast<unsigned char>(value)) || value == '_';
}

bool matching(char open, char close) {
	return (open == '(' && close == ')') || (open == '[' && close == ']') || (open == '{' && close == '}');
}

ScanResult scan_to(std::string_view source, size_t offset) {
	offset = std::min(offset, source.size());
	ScanResult result;
	result.code.assign(offset, true);
	bool comment = false;
	char quote = 0;
	bool triple = false;
	bool escaped = false;
	CaretLexicalContext string_kind = CaretLexicalContext::String;
	for (size_t index = 0; index < offset; ++index) {
		auto character = source[index];
		if (comment) {
			result.code[index] = false;
			if (character == '\n') {
				comment = false;
				if (result.stack.empty()) result.statement_start = index + 1;
			}
			continue;
		}
		if (quote) {
			result.code[index] = false;
			if (triple && character == quote && index + 2 < offset && source[index + 1] == quote &&
					source[index + 2] == quote) {
				result.code[index + 1] = false;
				result.code[index + 2] = false;
				index += 2;
				quote = 0;
				triple = false;
			} else if (escaped) {
				escaped = false;
			} else if (character == '\\') {
				escaped = true;
			} else if (!triple && character == quote) {
				quote = 0;
			}
			continue;
		}
		if (character == '#') {
			comment = true;
			result.code[index] = false;
			continue;
		}
		if (character == '\'' || character == '"') {
			quote = character;
			result.code[index] = false;
			string_kind = index > 0 && source[index - 1] == '&' ? CaretLexicalContext::StringName :
				(index > 0 && source[index - 1] == '^' ? CaretLexicalContext::NodePath : CaretLexicalContext::String);
			if (index + 2 < offset && source[index + 1] == quote && source[index + 2] == quote) {
				triple = true;
				result.code[index + 1] = false;
				result.code[index + 2] = false;
				index += 2;
			}
			continue;
		}
		if (character == '(' || character == '[' || character == '{') {
			result.stack.push_back({character, index, {}});
		} else if (character == ')' || character == ']' || character == '}') {
			for (auto it = result.stack.rbegin(); it != result.stack.rend(); ++it) {
				if (!matching(it->value, character)) continue;
				result.stack.erase(std::next(it).base(), result.stack.end());
				break;
			}
		} else if (character == ',' && !result.stack.empty() && result.stack.back().value == '(') {
			result.stack.back().commas.push_back(index);
		} else if (character == '\n' && result.stack.empty()) {
			result.statement_start = index + 1;
		}
	}
	result.quote = quote;
	result.lexical = comment ? CaretLexicalContext::Comment : (quote ? string_kind : CaretLexicalContext::Code);
	return result;
}

size_t previous_code_nonspace(std::string_view source, const std::vector<bool> &code, size_t end) {
	while (end > 0) {
		auto index = end - 1;
		if (index < code.size() && code[index] && !std::isspace(static_cast<unsigned char>(source[index]))) return index;
		--end;
	}
	return std::string_view::npos;
}

size_t expression_start(std::string_view source, const std::vector<bool> &code, size_t end) {
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	while (end > 0) {
		auto index = end - 1;
		if (index >= code.size() || !code[index]) {
			end = index;
			continue;
		}
		auto character = source[index];
		if (character == ')') ++parentheses;
		else if (character == ']') ++brackets;
		else if (character == '}') ++braces;
		else if (character == '(') {
			if (parentheses == 0) break;
			--parentheses;
		} else if (character == '[') {
			if (brackets == 0) break;
			--brackets;
		} else if (character == '{') {
			if (braces == 0) break;
			--braces;
		} else if (parentheses == 0 && brackets == 0 && braces == 0 &&
				(std::isspace(static_cast<unsigned char>(character)) || character == '=' || character == ',' ||
				 character == ':' || character == ';' || character == '+' || character == '-' || character == '*' ||
				 character == '/' || character == '%' || character == '!' || character == '<' || character == '>' ||
				 character == '&' || character == '|' || character == '^' || character == '?')) {
			break;
		}
		end = index;
	}
	return end;
}

std::string masked_text(std::string_view source, const std::vector<bool> &code, size_t begin, size_t end) {
	std::string result(source.substr(begin, end - begin));
	for (size_t index = begin; index < end && index < code.size(); ++index) {
		if (!code[index] && source[index] != '\n') result[index - begin] = ' ';
	}
	return result;
}

bool word_at(std::string_view source, const std::vector<bool> &code, size_t offset, std::string_view word) {
	if (offset + word.size() > code.size() || source.substr(offset, word.size()) != word) return false;
	for (size_t index = offset; index < offset + word.size(); ++index) if (!code[index]) return false;
	return (offset == 0 || !identifier_character(source[offset - 1])) &&
		(offset + word.size() == source.size() || !identifier_character(source[offset + word.size()]));
}

std::string identifier_after_keyword(std::string_view statement, std::string_view keyword) {
	auto clean = trim(statement);
	if (!clean.starts_with(keyword)) return {};
	clean = trim(std::string_view(clean).substr(keyword.size()));
	if (clean.empty() || !identifier_start(clean.front())) return {};
	size_t end = 1;
	while (end < clean.size() && identifier_character(clean[end])) ++end;
	return std::string(clean.substr(0, end));
}

bool simple_equal_at(std::string_view source, const std::vector<bool> &code, size_t index) {
	if (source[index] != '=' || !code[index]) return false;
	auto previous = index == 0 ? '\0' : source[index - 1];
	auto next = index + 1 < source.size() ? source[index + 1] : '\0';
	return previous != ':' && previous != '<' && previous != '>' && previous != '!' && previous != '=' &&
		previous != '+' && previous != '-' && previous != '*' && previous != '/' && previous != '%' &&
		previous != '&' && previous != '|' && previous != '^' && next != '=';
}

std::optional<size_t> root_assignment(std::string_view source, const std::vector<bool> &code,
		size_t begin, size_t end) {
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	std::optional<size_t> result;
	for (size_t index = begin; index < end; ++index) {
		if (index >= code.size() || !code[index]) continue;
		auto character = source[index];
		if (character == '(') ++parentheses;
		else if (character == ')') --parentheses;
		else if (character == '[') ++brackets;
		else if (character == ']') --brackets;
		else if (character == '{') ++braces;
		else if (character == '}') --braces;
		else if (parentheses == 0 && brackets == 0 && braces == 0 && simple_equal_at(source, code, index)) result = index;
	}
	return result;
}

std::optional<size_t> last_root_colon(std::string_view source, const std::vector<bool> &code,
		size_t begin, size_t end) {
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	std::optional<size_t> result;
	for (size_t index = begin; index < end; ++index) {
		if (index >= code.size() || !code[index]) continue;
		auto character = source[index];
		if (character == '(') ++parentheses;
		else if (character == ')') --parentheses;
		else if (character == '[') ++brackets;
		else if (character == ']') --brackets;
		else if (character == '{') ++braces;
		else if (character == '}') --braces;
		else if (character == ':' && parentheses == 0 && brackets == 0 && braces == 0) result = index;
	}
	return result;
}

bool type_tail(std::string_view value) {
	return std::all_of(value.begin(), value.end(), [](unsigned char character) {
		return std::isalnum(character) || character == '_' || character == '.' || character == '[' ||
			character == ']' || character == ',' || std::isspace(character);
	});
}

bool detect_type_hint(std::string_view source, const std::vector<bool> &code, size_t begin, size_t end) {
	auto statement = masked_text(source, code, begin, end);
	auto clean = trim(statement);
	if (clean.empty()) return false;
	if (clean.starts_with("extends ")) return type_tail(trim(std::string_view(clean).substr(7)));
	auto extends = clean.rfind(" extends ");
	if (extends != std::string::npos) {
		return type_tail(trim(std::string_view(clean).substr(extends + 9)));
	}
	for (auto keyword : {std::string_view(" as "), std::string_view(" is ")}) {
		auto marker = statement.rfind(keyword);
		if (marker != std::string::npos && type_tail(trim(std::string_view(statement).substr(marker + keyword.size())))) return true;
	}
	auto arrow = statement.rfind("->");
	if (arrow != std::string::npos && type_tail(trim(std::string_view(statement).substr(arrow + 2)))) return true;

	auto colon = statement.rfind(':');
	if (colon == std::string::npos || (colon + 1 < statement.size() && statement[colon + 1] == '=')) return false;
	auto equal = root_assignment(source, code, begin, end);
	if (equal) return false;
	if (!type_tail(trim(std::string_view(statement).substr(colon + 1)))) return false;
	auto before = trim(std::string_view(statement).substr(0, colon));
	if (before.starts_with("var ") || before.starts_with("const ") || before.starts_with("@export var ") ||
			before.starts_with("@onready var ")) return true;
	// Parameter annotations may be nested inside a function/lambda parameter list.
	auto function = statement.find("func");
	if (function != std::string::npos && statement.find('(', function + 4) != std::string::npos) return true;
	auto signal = statement.find("signal");
	return signal != std::string::npos && statement.find('(', signal + 6) != std::string::npos;
}

std::string strip_control_prefix(std::string value) {
	value = trim(value);
	for (auto prefix : {std::string_view("if "), std::string_view("elif "), std::string_view("while "),
			std::string_view("assert "), std::string_view("return ")}) {
		if (value.starts_with(prefix)) return trim(std::string_view(value).substr(prefix.size()));
	}
	return value;
}

std::optional<CaretOperationContext> operation_at(std::string_view source, const std::vector<bool> &code,
		size_t begin, size_t end) {
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	size_t clause_start = begin;
	std::optional<CaretOperationContext> result;
	auto set_operation = [&](size_t index, size_t length) {
		result = CaretOperationContext{
			std::string(source.substr(index, length)),
			strip_control_prefix(std::string(source.substr(clause_start, index - clause_start))),
			trim(source.substr(index + length, end - index - length)),
		};
	};
	for (size_t index = begin; index < end; ++index) {
		if (index >= code.size() || !code[index]) continue;
		auto character = source[index];
		if (character == '(') ++parentheses;
		else if (character == ')') --parentheses;
		else if (character == '[') ++brackets;
		else if (character == ']') --brackets;
		else if (character == '{') ++braces;
		else if (character == '}') --braces;
		if (parentheses != 0 || brackets != 0 || braces != 0) continue;
		if (simple_equal_at(source, code, index)) {
			clause_start = index + 1;
			result.reset();
			continue;
		}
		if (word_at(source, code, index, "and") || word_at(source, code, index, "or")) {
			auto length = source.compare(index, 3, "and") == 0 ? 3U : 2U;
			clause_start = index + length;
			result.reset();
			index += length - 1;
			continue;
		}
		size_t length = 0;
		if (index + 1 < end && (source.substr(index, 2) == "==" || source.substr(index, 2) == "!=" ||
				source.substr(index, 2) == "<=" || source.substr(index, 2) == ">=" ||
				source.substr(index, 2) == "+=" || source.substr(index, 2) == "-=" ||
				source.substr(index, 2) == "*=" || source.substr(index, 2) == "/=" ||
				source.substr(index, 2) == "%=" || source.substr(index, 2) == "&=" ||
				source.substr(index, 2) == "|=" || source.substr(index, 2) == "^=")) length = 2;
		else if (character == '<' || character == '>' || character == '+' || character == '-' ||
				character == '*' || character == '/' || character == '%' || character == '&' ||
				character == '|' || character == '^') length = 1;
		if (length) {
			set_operation(index, length);
			index += length - 1;
		}
	}
	return result;
}

const SyntaxNode *field(const SyntaxNode &node, std::string_view name) {
	for (const auto &child : node.children) if (child.field == name) return &child;
	return nullptr;
}

bool contains_byte(const SyntaxNode &node, size_t offset) {
	return node.start_byte <= offset && offset <= node.end_byte;
}

bool structural_conditional(const Document &document, const SyntaxNode &node, size_t offset,
		CaretConditionalContext &conditional) {
	if (!contains_byte(node, offset)) return false;
	if (node.kind == "conditional_expression") {
		auto *left = field(node, "left");
		auto *condition = field(node, "condition");
		auto *right = field(node, "right");
		if (left) conditional.true_expression = trim(document.text(*left));
		if (condition) conditional.condition_expression = trim(document.text(*condition));
		if (right) conditional.false_expression = trim(document.text(*right));
		if (left && contains_byte(*left, offset)) conditional.branch = ConditionalBranch::TrueValue;
		else if (condition && contains_byte(*condition, offset)) conditional.branch = ConditionalBranch::Condition;
		else if (right && contains_byte(*right, offset)) conditional.branch = ConditionalBranch::FalseValue;
		if (conditional.branch != ConditionalBranch::None) return true;
	}
	for (const auto &child : node.children) if (structural_conditional(document, child, offset, conditional)) return true;
	return false;
}

std::optional<CaretConditionalContext> scanned_conditional(std::string_view source,
		const std::vector<bool> &code, size_t begin, size_t end) {
	int parentheses = 0;
	int brackets = 0;
	int braces = 0;
	std::optional<size_t> if_at;
	std::optional<size_t> else_at;
	for (size_t index = begin; index < end; ++index) {
		if (!code[index]) continue;
		auto character = source[index];
		if (character == '(') ++parentheses;
		else if (character == ')') --parentheses;
		else if (character == '[') ++brackets;
		else if (character == ']') --brackets;
		else if (character == '{') ++braces;
		else if (character == '}') --braces;
		if (parentheses || brackets || braces) continue;
		if (word_at(source, code, index, "if")) {
			auto before = trim(masked_text(source, code, begin, index));
			if (!before.empty() && !before.starts_with("if ") && !before.starts_with("elif ")) if_at = index;
		} else if (if_at && word_at(source, code, index, "else")) {
			else_at = index;
		}
	}
	if (!if_at) return std::nullopt;
	CaretConditionalContext result;
	result.true_expression = trim(source.substr(begin, *if_at - begin));
	if (else_at) {
		result.condition_expression = trim(source.substr(*if_at + 2, *else_at - *if_at - 2));
		result.false_expression = trim(source.substr(*else_at + 4, end - *else_at - 4));
		result.branch = ConditionalBranch::FalseValue;
	} else {
		result.condition_expression = trim(source.substr(*if_at + 2, end - *if_at - 2));
		result.branch = ConditionalBranch::Condition;
	}
	return result;
}

std::string enclosing_match(std::string_view source, size_t offset) {
	auto indentation = [](std::string_view line) {
		size_t width = 0;
		for (auto character : line) {
			if (character == ' ') ++width;
			else if (character == '\t') width = (width / 4 + 1) * 4;
			else break;
		}
		return width;
	};
	auto line_start = source.rfind('\n', offset == 0 ? 0 : offset - 1);
	line_start = line_start == std::string_view::npos ? 0 : line_start + 1;
	auto current = source.substr(line_start, offset - line_start);
	auto indent = indentation(current);
	while (line_start > 0) {
		auto end = line_start - 1;
		auto begin = end == 0 ? std::string_view::npos : source.rfind('\n', end - 1);
		begin = begin == std::string_view::npos ? 0 : begin + 1;
		auto line = source.substr(begin, end - begin);
		auto clean = trim(line);
		if (!clean.empty() && !clean.starts_with('#')) {
			auto line_indent = indentation(line);
			if (line_indent < indent) {
				if (clean.starts_with("match ") && clean.ends_with(':')) {
					return trim(clean.substr(6, clean.size() - 7));
				}
				return {};
			}
		}
		line_start = begin;
	}
	return {};
}

} // namespace

CaretContext analyze_caret(const Document &document, Position position) {
	CaretContext result;
	auto &source = document.source();
	result.byte_offset = position_to_byte(source, position);
	auto scan = scan_to(source, result.byte_offset);
	result.lexical = scan.lexical;
	result.statement_start = scan.statement_start;

	// Keep call information even in strings: member-string providers need the
	// containing callable and argument index.
	for (auto delimiter = scan.stack.rbegin(); delimiter != scan.stack.rend(); ++delimiter) {
		if (delimiter->value != '(') continue;
		auto previous = previous_code_nonspace(source, scan.code, delimiter->offset);
		if (previous == std::string_view::npos) continue;
		auto begin = expression_start(source, scan.code, delimiter->offset);
		auto callee = trim(std::string_view(source).substr(begin, delimiter->offset - begin));
		static const std::unordered_set<std::string> controls = {"if", "elif", "while", "for", "match"};
		if (callee.empty() || controls.contains(callee) ||
				!(identifier_character(source[previous]) || source[previous] == ')' || source[previous] == ']')) continue;
		CaretCallContext call;
		call.callee = std::move(callee);
		call.argument_index = delimiter->commas.size();
		call.in_string = scan.lexical == CaretLexicalContext::String ||
			scan.lexical == CaretLexicalContext::StringName || scan.lexical == CaretLexicalContext::NodePath;
		call.quote = scan.quote;
		size_t argument_start = delimiter->offset + 1;
		for (auto comma : delimiter->commas) {
			call.arguments.push_back(trim(std::string_view(source).substr(argument_start, comma - argument_start)));
			argument_start = comma + 1;
		}
		call.arguments.push_back(trim(std::string_view(source).substr(argument_start, result.byte_offset - argument_start)));
		result.call = std::move(call);
		break;
	}

	result.in_type_hint = result.lexical == CaretLexicalContext::Code &&
		detect_type_hint(source, scan.code, result.statement_start, result.byte_offset);

	if (auto equal = root_assignment(source, scan.code, result.statement_start, result.byte_offset)) {
		result.assignment_left = trim(std::string_view(source).substr(result.statement_start, *equal - result.statement_start));
		result.assignment_right = trim(std::string_view(source).substr(*equal + 1, result.byte_offset - *equal - 1));
		result.suppressed_symbol = identifier_after_keyword(result.assignment_left, "var");
		if (result.suppressed_symbol.empty()) result.suppressed_symbol = identifier_after_keyword(result.assignment_left, "const");
		if (result.suppressed_symbol.empty() && !result.assignment_left.empty() &&
				identifier_start(result.assignment_left.front()) &&
				std::all_of(result.assignment_left.begin(), result.assignment_left.end(), identifier_character)) {
			result.suppressed_symbol = result.assignment_left;
		}
		if (auto colon = result.assignment_left.rfind(':'); colon != std::string::npos) {
			result.declared_type = trim(std::string_view(result.assignment_left).substr(colon + 1));
		}
	} else {
		auto statement = masked_text(source, scan.code, result.statement_start, result.byte_offset);
		result.suppressed_symbol = identifier_after_keyword(statement, "var");
		if (result.suppressed_symbol.empty()) result.suppressed_symbol = identifier_after_keyword(statement, "const");
	}

	size_t expression_scope = result.statement_start;
	auto root_colon = last_root_colon(source, scan.code, result.statement_start, result.byte_offset);
	// A root colon ends a control-flow header or match pattern. Anything after
	// it belongs to the suite, so comparisons in the header must not keep
	// supplying expected values there. Colons inside dictionaries, subscripts,
	// and parameter lists are nested and intentionally do not form a boundary.
	if (root_colon) expression_scope = *root_colon + 1;
	if (!scan.stack.empty()) expression_scope = scan.stack.back().offset + 1;
	result.operation = operation_at(source, scan.code, expression_scope, result.byte_offset);

	CaretConditionalContext structural;
	if (structural_conditional(document, document.syntax_root(), result.byte_offset, structural)) {
		result.conditional = std::move(structural);
	} else {
		result.conditional = scanned_conditional(source, scan.code, expression_scope, result.byte_offset);
	}

	if (result.lexical == CaretLexicalContext::Code) {
		size_t prefix_start = result.byte_offset;
		while (prefix_start > 0 && scan.code[prefix_start - 1] && identifier_character(source[prefix_start - 1])) --prefix_start;
		if (prefix_start > 0 && scan.code[prefix_start - 1] && source[prefix_start - 1] == '.') {
			result.member_access = true;
			auto dot = prefix_start - 1;
			auto begin = expression_start(source, scan.code, dot);
			auto receiver = trim(std::string_view(source).substr(begin, dot - begin));
			if (!receiver.empty()) result.member_receiver = std::move(receiver);
			result.member_prefix = std::string(std::string_view(source).substr(prefix_start, result.byte_offset - prefix_start));
		}
	}

	// On a match-arm line the root colon is the boundary between the pattern
	// and its body. Keep the subject only while the caret is still in the
	// pattern, including incomplete patterns that do not have a colon yet.
	if (!root_colon) result.match_expression = enclosing_match(source, result.byte_offset);
	if (result.lexical != CaretLexicalContext::Code) return result;
	auto previous = previous_code_nonspace(source, scan.code, result.byte_offset);
	auto immediately_after_root_colon = root_colon && previous == *root_colon;
	if (result.in_type_hint) result.role = CaretRole::TypeHint;
	else if (result.member_access) result.role = CaretRole::MemberAccess;
	else if (immediately_after_root_colon) result.role = CaretRole::Suppressed;
	else if (result.conditional) {
		switch (result.conditional->branch) {
			case ConditionalBranch::TrueValue: result.role = CaretRole::ConditionalTrue; break;
			case ConditionalBranch::Condition: result.role = CaretRole::ConditionalCondition; break;
			case ConditionalBranch::FalseValue: result.role = CaretRole::ConditionalFalse; break;
			default: break;
		}
	} else if (result.operation && (result.operation->operation == "==" || result.operation->operation == "!=" ||
			result.operation->operation == "<" || result.operation->operation == "<=" ||
			result.operation->operation == ">" || result.operation->operation == ">=")) {
		result.role = CaretRole::ComparisonRight;
	} else if (!scan.stack.empty() && scan.stack.back().value == '[') {
		auto previous = previous_code_nonspace(source, scan.code, scan.stack.back().offset);
		result.role = previous != std::string_view::npos &&
			(identifier_character(source[previous]) || source[previous] == ')' || source[previous] == ']') ?
			CaretRole::IndexAccess : CaretRole::ArrayElement;
	}
	else if (!scan.stack.empty() && scan.stack.back().value == '{') result.role = CaretRole::DictionaryEntry;
	else if (result.call) result.role = CaretRole::CallArgument;
	else if (!result.assignment_left.empty()) result.role = CaretRole::AssignmentValue;
	else if (!result.match_expression.empty()) result.role = CaretRole::MatchPattern;
	return result;
}

} // namespace gdscript_lsp
