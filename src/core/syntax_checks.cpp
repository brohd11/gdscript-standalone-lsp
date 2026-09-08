#include "core/syntax_checks.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace gdscript_lsp {
namespace {
const SyntaxNode *field(const SyntaxNode &node, std::string_view key) {
	for (const auto &child : node.children) if (child.field == key) return &child;
	return nullptr;
}
bool has_void(std::string_view value) {
	size_t at = 0;
	while ((at = value.find("void", at)) != std::string_view::npos) {
		if ((at == 0 || !identifier_byte(value[at - 1])) &&
			(at + 4 == value.size() || !identifier_byte(value[at + 4]))) return true;
		at += 4;
	}
	return false;
}

const SyntaxNode *first_descendant(const SyntaxNode &node, std::string_view kind) {
	if (node.kind == kind) return &node;
	for (const auto &child : node.children) {
		if (auto *found = first_descendant(child, kind)) return found;
	}
	return nullptr;
}

void descendants(const SyntaxNode &node, std::string_view kind, std::vector<const SyntaxNode *> &result) {
	if (node.kind == kind) result.push_back(&node);
	for (const auto &child : node.children) descendants(child, kind, result);
}

std::string node_name(const SyntaxNode &node, const Document &document) {
	if (node.kind == "identifier" || node.kind == "name") return std::string(trim(document.text(node)));
	if (auto *name = field(node, "name")) return std::string(trim(document.text(*name)));
	if (auto *identifier = first_descendant(node, "identifier")) return std::string(trim(document.text(*identifier)));
	if (auto *name = first_descendant(node, "name")) return std::string(trim(document.text(*name)));
	return {};
}

Range prefix_range(const Document &document, const SyntaxNode &node, size_t length) {
	auto end = std::min<size_t>(node.start_byte + length, node.end_byte);
	return {node.range.start, byte_to_position(document.source(), end)};
}

bool contains_kind(const SyntaxNode &node, std::string_view kind) {
	return first_descendant(node, kind) != nullptr;
}

bool return_has_value(const SyntaxNode &node, const Document &document) {
	auto value = trim(document.text(node));
	return value.size() > 6 && value.substr(6).find_first_not_of(" \t\r\n") != std::string_view::npos;
}

const SyntaxNode *return_with_value(const SyntaxNode &node, const Document &document, bool root = true) {
	if (!root && (node.kind == "function_definition" || node.kind == "constructor_definition" || node.kind == "lambda")) return nullptr;
	if (node.kind == "return_statement" && return_has_value(node, document)) return &node;
	for (const auto &child : node.children) {
		if (auto *found = return_with_value(child, document, false)) return found;
	}
	return nullptr;
}

void inspect_local_scopes(const SyntaxNode &body, const Document &document,
		const std::unordered_set<std::string> &enclosing, std::vector<ParseIssue> &result) {
	std::unordered_set<std::string> local;
	auto visible = enclosing;
	auto add = [&](std::string message, Range range) { result.push_back({range, std::move(message)}); };
	std::function<void(const SyntaxNode &, const std::unordered_set<std::string> &)> visit_statement;
	visit_statement = [&](const SyntaxNode &node, const std::unordered_set<std::string> &scope) {
		if (node.kind == "function_definition" || node.kind == "constructor_definition" || node.kind == "lambda") return;
		auto nested_scope = scope;
		if (node.kind == "for_statement") {
			if (auto *left = field(node, "left")) {
				auto name = node_name(*left, document);
				if (!name.empty() && scope.contains(name)) {
					add("There is already a variable named \"" + name + "\" declared in this scope.", left->range);
				}
				if (!name.empty()) nested_scope.insert(std::move(name));
			}
		}
		for (const auto &child : node.children) {
			if (child.kind == "body") inspect_local_scopes(child, document, nested_scope, result);
			else visit_statement(child, nested_scope);
		}
	};
	for (const auto &statement : body.children) {
		if (statement.kind == "variable_statement" || statement.kind == "const_statement") {
			if (auto *name_node = field(statement, "name")) {
				auto name = node_name(*name_node, document);
				if (!name.empty() && !local.insert(name).second) {
					add("There is already a variable named \"" + name + "\" declared in this scope.", name_node->range);
				}
				if (!name.empty()) visible.insert(std::move(name));
			}
		}
		visit_statement(statement, visible);
	}
}

std::string class_body_token(const SyntaxNode &statement, const Document &document) {
	if (statement.kind == "expression_statement" && !statement.children.empty()) {
		auto kind = statement.children.front().kind;
		if (kind == "identifier") return "Identifier";
		if (kind == "integer" || kind == "float" || kind == "string" || kind == "true" ||
				kind == "false" || kind == "null" || kind == "array" || kind == "dictionary") return "Literal";
		if (kind == "assignment") return "=";
	}
	auto value = trim(document.text(statement));
	if (auto end = value.find_first_of(" \t\r\n:("); end != std::string_view::npos) value = value.substr(0, end);
	return value.empty() ? std::string(statement.kind) : std::string(value);
}
}

std::vector<ParseIssue> structural_issues(const Document &document) {
	std::vector<ParseIssue> result;
	std::function<void(const SyntaxNode &, unsigned, std::string_view)> visit = [&](const SyntaxNode &node, unsigned loops, std::string_view parent) {
		auto add = [&](std::string message, Range range) { result.push_back({range, std::move(message)}); };
		if (node.kind == "source" || node.kind == "class_body") {
			const SyntaxNode *class_name = nullptr;
			const SyntaxNode *extends = nullptr;
			for (const auto &statement : node.children) {
				if (statement.kind == "class_name_statement") {
					if (class_name) add(R"("class_name" can only be used once.)", statement.range);
					else class_name = &statement;
				} else if (statement.kind == "extends_statement") {
					if (extends) add(R"("extends" can only be used once.)", statement.range);
					else extends = &statement;
				} else if (statement.kind == "expression_statement") {
					auto text = trim(document.text(statement));
					if (text == "tool") add(R"(The "tool" keyword was removed in Godot 4. Use the "@tool" annotation instead.)", statement.range);
					else add("Unexpected \"" + class_body_token(statement, document) + "\" in class body.", statement.range);
				} else if (statement.kind == "if_statement" || statement.kind == "while_statement" ||
						statement.kind == "for_statement" || statement.kind == "match_statement" ||
						statement.kind == "return_statement" || statement.kind == "break_statement" ||
						statement.kind == "continue_statement") {
					add("Unexpected \"" + class_body_token(statement, document) + "\" in class body.", statement.range);
				}
			}
		}
		if (node.kind == "export_variable_statement") {
			add(R"(The "export" keyword was removed in Godot 4. Use an export annotation ("@export", "@export_range", etc.) instead.)", prefix_range(document, node, 6));
		}
		if (node.kind == "onready_variable_statement") {
			add(R"(The "onready" keyword was removed in Godot 4. Use the "@onready" annotation instead.)", prefix_range(document, node, 7));
		}
		if (node.kind == "remote_keyword") {
			auto keyword = std::string(trim(document.text(node)));
			std::string message;
			if (keyword == "remote") message = R"(The "remote" keyword was removed in Godot 4. Use the "@rpc" annotation with "any_peer" instead.)";
			else if (keyword == "master") message = R"(The "master" keyword was removed in Godot 4. Use the "@rpc" annotation with "any_peer" and perform a check inside the function instead.)";
			else if (keyword == "puppet") message = R"(The "puppet" keyword was removed in Godot 4. Use the "@rpc" annotation with "authority" instead.)";
			else if (keyword == "remotesync") message = R"(The "remotesync" keyword was removed in Godot 4. Use the "@rpc" annotation with "any_peer" and "call_local" instead.)";
			else if (keyword == "mastersync") message = R"(The "mastersync" keyword was removed in Godot 4. Use the "@rpc" annotation with "any_peer" and "call_local", and perform a check inside the function instead.)";
			else if (keyword == "puppetsync") message = R"(The "puppetsync" keyword was removed in Godot 4. Use the "@rpc" annotation with "authority" and "call_local" instead.)";
			if (!message.empty()) add(std::move(message), node.range);
		}
		if (node.kind == "setget" && trim(document.text(node)).starts_with("setget")) {
			add(R"(Expected end of statement after variable declaration, found "Identifier" instead.)", node.range);
		}
		if (node.kind == "function_definition" || node.kind == "constructor_definition" || node.kind == "lambda") {
			loops = 0;
			if (node.kind == "function_definition" && !field(node, "name")) {
				add("Standalone lambdas cannot be accessed; assign the lambda or pass it as an argument.",
					{node.range.start, byte_to_position(document.source(), node.start_byte + 4)});
			}
		}
		if (node.kind == "constructor_definition") {
			if (auto *return_type = field(node, "return_type"); return_type && trim(document.text(*return_type)) != "void") {
				add("Constructor cannot return a value.", return_type->range);
			} else if (auto *statement = return_with_value(node, document)) {
				add("Constructor cannot return a value.", statement->range);
			}
		}
		if (node.kind == "function_definition" && field(node, "name") && trim(document.text(*field(node, "name"))) == "_static_init") {
			if (auto *parameters = field(node, "parameters"); parameters && !parameters->children.empty()) {
				add("Static constructor cannot have parameters.", parameters->range);
			}
		}
		if (node.kind == "function_definition" && contains_kind(node, "static_keyword")) {
			std::function<void(const SyntaxNode &, bool)> inspect_self = [&](const SyntaxNode &part, bool root) {
				if (!root && (part.kind == "function_definition" || part.kind == "constructor_definition")) return;
				if (part.kind == "identifier" && trim(document.text(part)) == "self") add(R"(Cannot use "self" inside a static function.)", part.range);
				for (const auto &child : part.children) inspect_self(child, false);
			};
			inspect_self(node, true);
		}
		if ((node.kind == "break_statement" || node.kind == "continue_statement") && !loops) {
			add("Cannot use \"" + std::string(node.kind == "break_statement" ? "break" : "continue") + "\" outside of a loop.", node.range);
		}
		if (node.kind == "parameters") {
			bool optional = false;
			std::unordered_set<std::string> names;
			for (const auto &parameter : node.children) {
				if (parameter.kind == "comment" || parameter.has_error) continue;
				auto name = node_name(parameter, document);
				if (!name.empty() && !names.insert(name).second) {
					add("Parameter with name \"" + name + "\" was already declared for this " +
						(parent == "signal_statement" ? "signal." : "function."), parameter.range);
				}
				if (parent == "signal_statement" && field(parameter, "value")) {
					add("Signal parameters cannot have a default value.", parameter.range);
				}
				if (field(parameter, "value")) optional = true;
				else if (optional && parameter.kind != "variadic_parameter" && parent != "signal_statement") add("Required parameters cannot follow parameters with default values.", parameter.range);
			}
		}
		if ((node.kind == "if_statement" || node.kind == "while_statement") &&
				(!field(node, "condition") || trim(document.text(*field(node, "condition"))).empty())) {
			add("Expected expression after \"" + std::string(node.kind == "if_statement" ? "if" : "while") + "\".", node.range);
		}
		if (node.kind == "for_statement" && (!field(node, "right") || trim(document.text(*field(node, "right"))).empty())) {
			add(R"(Expected iterable expression after "in".)", node.range);
		}
		if (node.kind == "match_statement" && (!field(node, "value") || trim(document.text(*field(node, "value"))).empty())) {
			add(R"(Expected expression after "match".)", node.range);
		}
		if (node.kind == "enum_definition") {
			std::vector<const SyntaxNode *> enumerators;
			descendants(node, "enumerator", enumerators);
			std::unordered_map<std::string, unsigned> names;
			for (auto *enumerator : enumerators) {
				auto *left = field(*enumerator, "left");
				if (!left) continue;
				auto name = node_name(*left, document);
				auto [first, inserted] = names.emplace(name, left->range.start.line + 1);
				if (!name.empty() && !inserted) add("Name \"" + name + "\" was already in this enum (at line " + std::to_string(first->second) + ").", left->range);
			}
		}
		if (node.kind == "assignment") {
			if (auto *left = field(node, "left"); left && left->kind != "identifier" && left->kind != "name" &&
					left->kind != "attribute" && left->kind != "subscript") {
				add("Only identifier, attribute access, and subscription access can be used as assignment target.", left->range);
			}
		}
		if ((node.field == "type" || node.field == "return_type") && contains_kind(node, "subscript")) {
			auto *outer = first_descendant(node, "subscript");
			if (outer) {
				for (const auto &child : outer->children) {
					if (contains_kind(child, "subscript")) {
						add("Nested typed collections are not supported.", child.range);
						break;
					}
				}
				if (auto *arguments = field(*outer, "arguments"); arguments &&
						(trim(document.text(*arguments)).empty() ||
						(first_descendant(*arguments, "identifier") && trim(document.text(*first_descendant(*arguments, "identifier"))).empty()))) {
					add(R"(Expected type for collection after "[".)", arguments->range);
				}
			}
		}
		if (node.kind == "pattern_section") {
			std::vector<const SyntaxNode *> bindings;
			descendants(node, "pattern_binding", bindings);
			size_t patterns = 0;
			for (const auto &child : node.children) if (child.field != "body" && child.kind != "comment" && child.kind != "annotation") ++patterns;
			if (patterns > 1 && !bindings.empty()) add("Cannot use a variable bind with multiple patterns.", bindings.front()->range);
			std::unordered_set<std::string> names;
			for (auto *binding : bindings) {
				auto name = node_name(*binding, document);
				if (!name.empty() && !names.insert(name).second) add("Bind variable name \"" + name + "\" was already used in this pattern.", binding->range);
			}
		}
		if ((node.field == "type" || node.field == "return_type") && has_void(document.text(node)) &&
			!(node.field == "return_type" && trim(document.text(node)) == "void")) {
			add("The type \"void\" is only allowed as a function return type.", node.range);
			return;
		}
		if (node.kind == "expression_statement" && trim(document.text(node)) == "void") {
			add(R"(Expected statement, found "void" instead.)", node.range);
			return;
		}
		if (node.kind == "call") {
			for (const auto &child : node.children) if (child.kind == "identifier" && document.text(child) == "yield") {
				add("The function \"yield\" was removed in Godot 4. Use \"await\" instead.", child.range);
			}
		}
		for (const auto &child : node.children) {
			auto child_loops = loops;
			if ((node.kind == "for_statement" || node.kind == "while_statement") && child.field == "body") ++child_loops;
			visit(child, child_loops, node.kind);
		}
	};
	visit(document.syntax_root(), 0, {});
	std::function<void(const SyntaxNode &)> local_scopes = [&](const SyntaxNode &node) {
		if (node.kind == "function_definition" || node.kind == "constructor_definition" || node.kind == "lambda") {
			if (auto *body = field(node, "body")) inspect_local_scopes(*body, document, {}, result);
			for (const auto &child : node.children) local_scopes(child);
			return;
		}
		for (const auto &child : node.children) local_scopes(child);
	};
	local_scopes(document.syntax_root());
	return result;
}

std::vector<ParseIssue> lexical_issues(const Document &document) {
	std::vector<ParseIssue> result;
	auto add = [&](std::string message, Range range) { result.push_back({range, std::move(message)}); };
	std::function<void(const SyntaxNode &)> escapes = [&](const SyntaxNode &node) {
		if (node.kind == "escape_sequence") {
			auto value = document.text(node);
			auto all_digits = [&](size_t begin, size_t count, int base) {
				if (value.size() != begin + count) return false;
				for (size_t i = begin; i < value.size(); ++i) {
					auto c = static_cast<unsigned char>(value[i]);
					if (base == 16 ? !std::isxdigit(c) : c < '0' || c > '7') return false;
				}
				return true;
			};
			bool valid = value.size() == 2 && std::string_view("abfnrtv\"'\\").find(value[1]) != std::string_view::npos;
			valid |= value == "\\\n" || value == "\\\r\n";
			if (value.starts_with("\\u")) {
				if (!all_digits(2, 4, 16)) add("Invalid hexadecimal digit in unicode escape sequence.", node.range);
				valid = true;
			} else if (value.starts_with("\\U")) {
				if (!all_digits(2, 6, 16)) add("Invalid hexadecimal digit in unicode escape sequence.", node.range);
				valid = true;
			} else if (value.starts_with("\\x")) valid = all_digits(2, 2, 16);
			else if (value.starts_with("\\o")) valid = all_digits(2, 3, 8);
			if (!valid) add("Invalid escape in string.", node.range);
		}
		for (const auto &child : node.children) escapes(child);
	};
	escapes(document.syntax_root());

	std::vector<std::pair<std::string, unsigned>> indents{{"", 0}};
	auto source = std::string_view(document.source());
	size_t offset = 0;
	bool continuation = false;
	int delimiters = 0;
	char quote = 0;
	bool triple = false;
	uint32_t line_number = 0;
	while (offset < source.size()) {
		auto end = source.find('\n', offset);
		if (end == std::string_view::npos) end = source.size();
		auto line = source.substr(offset, end - offset);
		size_t indent_bytes = 0;
		while (indent_bytes < line.size() && (line[indent_bytes] == ' ' || line[indent_bytes] == '\t')) ++indent_bytes;
		auto code = line.substr(indent_bytes);
		bool ignored = code.empty() || code.front() == '#' || continuation || quote != 0;
		if (!ignored) {
			auto plain = trim(code.substr(0, code.find('#')));
			if (plain == "@") {
				add(R"(Expected annotation identifier after "@".)",
					{{line_number, static_cast<uint32_t>(indent_bytes)}, {line_number, static_cast<uint32_t>(indent_bytes + 1)}});
			}
			if (plain.ends_with(" is") || plain.ends_with(" is not") || plain.find(" is:") != std::string_view::npos || plain.find(" is not:") != std::string_view::npos) {
				auto at = plain.rfind("is");
				add(R"(Expected type specifier after "is".)",
					{{line_number, static_cast<uint32_t>(indent_bytes + at)}, {line_number, static_cast<uint32_t>(indent_bytes + at + 2)}});
			}
		}
		if (!ignored) {
			auto indentation = std::string(line.substr(0, indent_bytes));
			auto width = static_cast<unsigned>(indent_bytes);
			if (width > indents.back().second) indents.emplace_back(indentation, width);
			else if (width < indents.back().second) {
				auto match = std::find_if(indents.rbegin(), indents.rend(), [&](const auto &level) {
					return level.second == width && level.first == indentation;
				});
				if (match == indents.rend() && indents.size() > 1) {
					add("Unindent doesn't match the previous indentation level.",
						{{line_number, 0}, {line_number, static_cast<uint32_t>(indent_bytes)}});
					while (indents.size() > 1 && indents.back().second > width) indents.pop_back();
					indents.emplace_back(std::move(indentation), width);
				} else {
					while (indents.size() > 1 && indents.back().second > width) indents.pop_back();
				}
			}
		}
		bool escaped = false;
		bool comment = false;
		for (size_t i = indent_bytes; i < line.size() && !comment; ++i) {
			char c = line[i];
			if (quote) {
				if (escaped) { escaped = false; continue; }
				if (c == '\\') { escaped = true; continue; }
				if (triple && i + 2 < line.size() && line[i] == quote && line[i + 1] == quote && line[i + 2] == quote) {
					quote = 0; triple = false; i += 2;
				} else if (!triple && c == quote) quote = 0;
				continue;
			}
			if (c == '#') { comment = true; continue; }
			if (c == '\'' || c == '"') {
				quote = c;
				triple = i + 2 < line.size() && line[i + 1] == c && line[i + 2] == c;
				if (triple) i += 2;
			} else if (c == '(' || c == '[' || c == '{') ++delimiters;
			else if ((c == ')' || c == ']' || c == '}') && delimiters > 0) --delimiters;
		}
		if (quote && !triple) quote = 0;
		auto meaningful = line.substr(0, line.find('#'));
		while (!meaningful.empty() && (meaningful.back() == ' ' || meaningful.back() == '\t' || meaningful.back() == '\r')) meaningful.remove_suffix(1);
		continuation = delimiters > 0 || (!meaningful.empty() && meaningful.back() == '\\') || triple;
		offset = end == source.size() ? source.size() : end + 1;
		++line_number;
	}
	return result;
}

bool catch_all_pattern(const SyntaxNode &section, const Document &document) {
	for (const auto &child : section.children) if (child.kind == "pattern_guard") return false;
	for (const auto &child : section.children) {
		if (child.field == "body" || child.kind == "annotation" || child.kind == "comment") continue;
		if (child.kind == "pattern_binding" || trim(document.text(child)) == "_") return true;
	}
	return false;
}

unsigned statement_flow(const SyntaxNode &node, const Document &document) {
	if (node.has_error) return FallsThrough;
	if (node.kind == "return_statement") return Returns;
	if (node.kind == "break_statement") return Breaks;
	if (node.kind == "continue_statement") return Continues;
	if (node.kind == "body") {
		unsigned result = FallsThrough;
		for (const auto &child : node.children) {
			if (!(result & FallsThrough)) break;
			result = (result & ~FallsThrough) | statement_flow(child, document);
		}
		return result;
	}
	if (node.kind == "if_statement") {
		unsigned result = FallsThrough;
		if (auto body = field(node, "body")) result = statement_flow(*body, document);
		bool has_else = false;
		for (const auto &alternative : node.children) if (alternative.field == "alternative") {
			has_else |= alternative.kind == "else_clause";
			if (auto body = field(alternative, "body")) result |= statement_flow(*body, document);
			else result |= FallsThrough;
		}
		return has_else ? result : result | FallsThrough;
	}
	if (node.kind == "match_statement") {
		unsigned result = 0;
		bool exhaustive = false;
		if (auto body = field(node, "body")) for (const auto &section : body->children) {
			if (section.kind != "pattern_section") continue;
			if (auto suite = field(section, "body")) result |= statement_flow(*suite, document);
			else result |= FallsThrough;
			if (catch_all_pattern(section, document)) { exhaustive = true; break; }
		}
		return exhaustive ? result : result | FallsThrough;
	}
	// A loop can run zero times; its break/continue exits never escape the loop.
	if (node.kind == "for_statement" || node.kind == "while_statement") {
		auto body = field(node, "body");
		return FallsThrough | (body ? statement_flow(*body, document) & Returns : 0);
	}
	return FallsThrough;
}
}
