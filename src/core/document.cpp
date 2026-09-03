#include "core/document.hpp"
#include "core/gdscript_api.hpp"
#include "core/text.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <limits>

extern "C" const TSLanguage *tree_sitter_gdscript(void);

namespace gdscript_lsp {
namespace {

std::string node_type(TSNode node) {
	return ts_node_is_null(node) ? std::string{} : std::string(ts_node_type(node));
}

std::string node_text(TSNode node, std::string_view source) {
	if (ts_node_is_null(node)) return {};
	auto start = static_cast<size_t>(ts_node_start_byte(node));
	auto end = static_cast<size_t>(ts_node_end_byte(node));
	if (start > source.size() || end > source.size() || start > end) return {};
	return std::string(source.substr(start, end - start));
}

TSNode field(TSNode node, const char *name) {
	if (ts_node_is_null(node)) return {};
	return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(std::strlen(name)));
}

Range node_range(TSNode node, std::string_view source) {
	if (ts_node_is_null(node)) return {};
	return {byte_to_position(source, ts_node_start_byte(node)), byte_to_position(source, ts_node_end_byte(node))};
}

size_t utf8_width(unsigned char value) {
	if ((value & 0x80U) == 0) return 1;
	if ((value & 0xE0U) == 0xC0U) return 2;
	if ((value & 0xF0U) == 0xE0U) return 3;
	if ((value & 0xF8U) == 0xF0U) return 4;
	return 1;
}

Position syntax_position(std::string_view source, uint32_t byte, TSPoint point) {
	uint32_t utf16_column = 0;
	auto offset = byte >= point.column ? static_cast<size_t>(byte - point.column) : 0;
	while (offset < byte && offset < source.size()) {
		auto width = std::min(utf8_width(static_cast<unsigned char>(source[offset])), static_cast<size_t>(byte) - offset);
		uint32_t codepoint = static_cast<unsigned char>(source[offset]);
		if (width > 1) {
			codepoint &= (1U << (7U - static_cast<unsigned>(width))) - 1U;
			for (size_t index = 1; index < width; ++index) {
				codepoint = (codepoint << 6U) | (static_cast<unsigned char>(source[offset + index]) & 0x3FU);
			}
		}
		utf16_column += codepoint > 0xFFFFU ? 2U : 1U;
		offset += width;
	}
	return {point.row, utf16_column};
}

Range syntax_range(TSNode node, std::string_view source) {
	auto start_byte = ts_node_start_byte(node);
	auto end_byte = ts_node_end_byte(node);
	return {syntax_position(source, start_byte, ts_node_start_point(node)),
		syntax_position(source, end_byte, ts_node_end_point(node))};
}

SyntaxNode syntax_node(TSNode node, std::string_view source, std::string_view field_name = {}) {
	SyntaxNode result;
	result.kind = ts_node_type(node);
	result.field = field_name;
	result.range = syntax_range(node, source);
	result.start_byte = ts_node_start_byte(node);
	result.end_byte = ts_node_end_byte(node);
	result.has_error = ts_node_has_error(node);
	for (uint32_t index = 0; index < ts_node_child_count(node); ++index) {
		auto child = ts_node_child(node, index);
		if (!ts_node_is_named(child)) continue;
		const char *child_field = ts_node_field_name_for_child(node, index);
		result.children.push_back(syntax_node(child, source, child_field ? child_field : ""));
	}
	return result;
}

TSNode first_descendant(TSNode node, std::string_view wanted) {
	if (node_type(node) == wanted) return node;
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		auto found = first_descendant(ts_node_named_child(node, i), wanted);
		if (!ts_node_is_null(found)) return found;
	}
	return {};
}

bool has_named_child(TSNode node, std::string_view wanted) {
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		if (node_type(ts_node_named_child(node, i)) == wanted) return true;
	}
	return false;
}

bool is_inferred_type(std::string_view value) {
	std::string compact;
	for (char character : value) if (!std::isspace(static_cast<unsigned char>(character))) compact += character;
	return compact == ":=";
}

std::string extends_text(TSNode node, std::string_view source) {
	if (ts_node_is_null(node)) return {};
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		auto child = ts_node_named_child(node, i);
		if (node_type(child) == "annotations") continue;
		return trim(node_text(child, source));
	}
	auto raw = trim(node_text(node, source));
	if (raw.starts_with("extends")) return trim(raw.substr(7));
	return raw;
}

struct DeclarationScan {
	size_t end = 0;
	size_t code_end = 0;
	std::optional<size_t> colon;
	std::optional<size_t> assignment;
	bool missing_type = false;
	bool missing_value = false;
	bool recovered_type = false;
	bool recovered_value = false;

	bool malformed() const {
		return missing_type || missing_value || recovered_type || recovered_value;
	}
};

// Tree-sitter intentionally recovers an incomplete declaration by borrowing a
// node from a later line. Symbols and diagnostics must not accept that recovered
// extent as part of the declaration. A newline is a hard boundary unless it is
// inside a string/group or explicitly continued.
DeclarationScan scan_declaration(TSNode node, std::string_view source) {
	DeclarationScan result;
	auto start = std::min(static_cast<size_t>(ts_node_start_byte(node)), source.size());
	auto limit = std::min(source.size(), start + 64 * 1024);
	result.end = limit;
	result.code_end = limit;
	int grouping = 0;
	char quote = 0;
	bool triple = false;
	bool escaped = false;
	bool comment = false;
	size_t last_non_space = start;
	for (size_t index = start; index < limit; ++index) {
		auto character = source[index];
		if (comment) {
			if (character != '\n') continue;
			comment = false;
			if (grouping == 0) {
				result.end = index;
				break;
			}
			continue;
		}
		if (quote) {
			if (triple) {
				if (character == quote && index + 2 < limit && source[index + 1] == quote &&
						source[index + 2] == quote) {
					quote = 0;
					triple = false;
					index += 2;
				}
				continue;
			}
			if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (character == quote) quote = 0;
			continue;
		}
		if (character == '\'' || character == '"') {
			quote = character;
			triple = index + 2 < limit && source[index + 1] == character && source[index + 2] == character;
			if (triple) index += 2;
			last_non_space = index;
			continue;
		}
		if (character == '#') {
			comment = true;
			if (grouping == 0) result.code_end = std::min(result.code_end, index);
			continue;
		}
		if (character == '(' || character == '[' || character == '{') ++grouping;
		else if ((character == ')' || character == ']' || character == '}') && grouping > 0) --grouping;
		if (character == '\n' || character == '\r') {
			if (grouping == 0 && (last_non_space >= source.size() || source[last_non_space] != '\\')) {
				result.end = index;
				result.code_end = std::min(result.code_end, index);
				break;
			}
			continue;
		}
		if (!std::isspace(static_cast<unsigned char>(character))) last_non_space = index;
	}
	result.code_end = std::min(result.code_end, result.end);

	auto name = field(node, "name");
	auto cursor = ts_node_is_null(name) ? start : static_cast<size_t>(ts_node_end_byte(name));
	int depth = 0;
	quote = 0;
	escaped = false;
	for (size_t index = cursor; index < result.code_end; ++index) {
		auto character = source[index];
		if (quote) {
			if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (character == quote) quote = 0;
			continue;
		}
		if (character == '\'' || character == '"') { quote = character; continue; }
		if (character == '(' || character == '[' || character == '{') { ++depth; continue; }
		if ((character == ')' || character == ']' || character == '}') && depth > 0) { --depth; continue; }
		if (depth != 0) continue;
		if (character == ':' && !result.colon && !result.assignment) {
			result.colon = index;
			if (index + 1 < result.code_end && source[index + 1] == '=') {
				result.assignment = index + 1;
				++index;
			}
		} else if (character == '=' && !result.assignment) {
			result.assignment = index;
			break;
		}
	}
	auto has_non_space = [&](size_t begin, size_t end) {
		for (auto index = begin; index < end; ++index) {
			if (!std::isspace(static_cast<unsigned char>(source[index]))) return true;
		}
		return false;
	};
	if (result.colon && (!result.assignment || *result.assignment != *result.colon + 1)) {
		auto type_end = result.assignment.value_or(result.code_end);
		result.missing_type = !has_non_space(*result.colon + 1, type_end);
		// An untyped property may use the colon solely to introduce get/set.
		if (result.missing_type && !ts_node_is_null(field(node, "setget"))) result.missing_type = false;
	}
	if (result.assignment) result.missing_value = !has_non_space(*result.assignment + 1, result.code_end);
	auto type_node = field(node, "type");
	if (result.colon && (!result.assignment || *result.assignment != *result.colon + 1) && !ts_node_is_null(type_node)) {
		result.recovered_type = ts_node_start_byte(type_node) >= result.end || ts_node_end_byte(type_node) > result.end;
	}
	auto value_node = field(node, "value");
	if (result.assignment && !ts_node_is_null(value_node)) {
		// Lambdas have a multiline body even though their value starts on the
		// declaration line. Recovery is only suspect when the value itself starts
		// beyond the lexical declaration boundary.
		result.recovered_value = ts_node_start_byte(value_node) >= result.end;
	}
	return result;
}

Symbol variable_symbol(TSNode node, const std::string &uri, const std::string &owner_id,
		std::string_view source, bool local) {
	Symbol symbol;
	auto name_node = field(node, "name");
	symbol.name = trim(node_text(name_node, source));
	symbol.id = owner_id + "::" + symbol.name;
	if (local) {
		auto start = syntax_position(source, ts_node_start_byte(name_node), ts_node_start_point(name_node));
		symbol.id += "@" + std::to_string(start.line) + ":" + std::to_string(start.character);
	}
	symbol.qualified_name = symbol.id;
	symbol.uri = uri;
	symbol.kind = node_type(node) == "const_statement" ? SymbolKind::Constant : SymbolKind::Variable;
	auto declaration = scan_declaration(node, source);
	symbol.range = declaration.malformed() ?
		Range{node_range(node, source).start, byte_to_position(source, declaration.end)} : node_range(node, source);
	symbol.selection_range = node_range(name_node, source);
	symbol.declared_type = declaration.recovered_type ? std::string{} : trim(node_text(field(node, "type"), source));
	symbol.is_inferred = is_inferred_type(symbol.declared_type);
	if (symbol.is_inferred) symbol.declared_type.clear();
	symbol.initializer = declaration.recovered_value ? std::string{} : trim(node_text(field(node, "value"), source));
	symbol.is_static = !ts_node_is_null(field(node, "static")) || has_named_child(node, "static_keyword");
	symbol.is_local = local;
	symbol.malformed = declaration.malformed() || ts_node_has_error(node);
	symbol.detail = (symbol.kind == SymbolKind::Constant ? "const " : "var ") + symbol.name;
	if (!symbol.declared_type.empty()) symbol.detail += ": " + symbol.declared_type;
	return symbol;
}

void collect_locals(TSNode node, Symbol &function, std::string_view source,
		size_t begin = 0, size_t end = std::numeric_limits<size_t>::max()) {
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		auto child = ts_node_named_child(node, i);
		auto child_start = static_cast<size_t>(ts_node_start_byte(child));
		auto child_end = static_cast<size_t>(ts_node_end_byte(child));
		if (child_end < begin || child_start >= end) continue;
		auto type = node_type(child);
		if ((type == "variable_statement" || type == "const_statement") && child_start >= begin) {
			auto local = variable_symbol(child, function.uri, function.id, source, true);
			auto malformed = local.malformed;
			function.children.push_back(std::move(local));
			if (malformed) continue;
		} else if (type == "for_statement" && child_start >= begin) {
			auto left = field(child, "left");
			Symbol local;
			local.name = trim(node_text(left, source));
			auto start = syntax_position(source, ts_node_start_byte(left), ts_node_start_point(left));
			local.id = function.id + "::" + local.name + "@" + std::to_string(start.line) + ":" +
				std::to_string(start.character);
			local.qualified_name = local.id;
			local.uri = function.uri;
			local.kind = SymbolKind::Variable;
			local.range = node_range(child, source);
			local.selection_range = node_range(left, source);
			local.declared_type = trim(node_text(field(child, "type"), source));
			local.is_inferred = is_inferred_type(local.declared_type);
			if (local.is_inferred) local.declared_type.clear();
			// Preserve the iterable for the standalone resolver. The flag keeps it
			// distinct from an ordinary assignment initializer.
			local.initializer = trim(node_text(field(child, "right"), source));
			local.is_iteration_variable = true;
			local.is_local = true;
			function.children.push_back(std::move(local));
		}
		collect_locals(child, function, source, begin, end);
	}
}

size_t line_end(std::string_view source, size_t byte) {
	auto found = source.find('\n', std::min(byte, source.size()));
	return found == std::string_view::npos ? source.size() : found;
}

size_t indentation_width(std::string_view line) {
	size_t width = 0;
	for (auto character : line) {
		if (character == ' ') ++width;
		else if (character == '\t') width = (width / 4 + 1) * 4;
		else break;
	}
	return width;
}

std::vector<bool> source_code_mask(std::string_view source) {
	std::vector<bool> code(source.size(), true);
	bool comment = false;
	char quote = 0;
	bool triple = false;
	bool escaped = false;
	for (size_t index = 0; index < source.size(); ++index) {
		auto character = source[index];
		if (comment) {
			code[index] = false;
			if (character == '\n') comment = false;
			continue;
		}
		if (quote) {
			code[index] = false;
			if (triple && character == quote && index + 2 < source.size() &&
					source[index + 1] == quote && source[index + 2] == quote) {
				code[index + 1] = false;
				code[index + 2] = false;
				index += 2;
				quote = 0;
				triple = false;
			} else if (escaped) escaped = false;
			else if (character == '\\') escaped = true;
			else if (!triple && character == quote) quote = 0;
			continue;
		}
		if (character == '#') {
			code[index] = false;
			comment = true;
		} else if (character == '\'' || character == '"') {
			code[index] = false;
			quote = character;
			if (index + 2 < source.size() && source[index + 1] == character && source[index + 2] == character) {
				triple = true;
				code[index + 1] = false;
				code[index + 2] = false;
				index += 2;
			}
		}
	}
	return code;
}

struct FunctionExtent {
	size_t keyword = 0;
	size_t name_start = 0;
	size_t name_end = 0;
	size_t header_end = 0;
	size_t body_start = 0;
	size_t end = 0;
	bool is_static = false;
	bool valid = false;
};

FunctionExtent function_extent(std::string_view source, const std::vector<bool> &code, size_t start) {
	FunctionExtent result;
	auto current_end = line_end(source, start);
	auto cursor = start;
	while (cursor < current_end && (source[cursor] == ' ' || source[cursor] == '\t')) ++cursor;
	if (cursor >= code.size() || !code[cursor]) return result;
	if (source.substr(cursor, 7) == "static ") {
		result.is_static = true;
		cursor += 7;
	}
	if (source.substr(cursor, 4) != "func" || (cursor + 4 < source.size() &&
			(std::isalnum(static_cast<unsigned char>(source[cursor + 4])) || source[cursor + 4] == '_'))) return result;
	result.keyword = cursor;
	cursor += 4;
	while (cursor < source.size() && std::isspace(static_cast<unsigned char>(source[cursor])) && source[cursor] != '\n') ++cursor;
	if (cursor >= source.size() || !(std::isalpha(static_cast<unsigned char>(source[cursor])) || source[cursor] == '_')) return result;
	result.name_start = cursor++;
	while (cursor < source.size() && (std::isalnum(static_cast<unsigned char>(source[cursor])) || source[cursor] == '_')) ++cursor;
	result.name_end = cursor;

	int grouping = 0;
	size_t colon = std::string_view::npos;
	auto limit = std::min(source.size(), start + 64 * 1024);
	for (size_t index = cursor; index < limit; ++index) {
		if (index >= code.size() || !code[index]) continue;
		auto character = source[index];
		if (character == '(' || character == '[' || character == '{') ++grouping;
		else if ((character == ')' || character == ']' || character == '}') && grouping > 0) --grouping;
		else if (character == ':' && grouping == 0) { colon = index; break; }
		else if (character == '\n' && grouping == 0) break;
	}
	if (colon == std::string_view::npos) return result;
	result.header_end = colon + 1;
	auto header_line_end = line_end(source, colon);
	auto tail = trim(source.substr(colon + 1, header_line_end - colon - 1));
	if (!tail.empty() && !tail.starts_with('#')) {
		result.body_start = colon + 1;
		result.end = header_line_end;
		result.valid = true;
		return result;
	}
	result.body_start = header_line_end < source.size() ? header_line_end + 1 : source.size();
	auto header_indent = indentation_width(source.substr(start, result.keyword - start));
	result.end = source.size();
	for (size_t next = result.body_start; next < source.size();) {
		auto next_end = line_end(source, next);
		auto line = source.substr(next, next_end - next);
		auto clean = trim(line);
		if (!clean.empty() && !clean.starts_with('#') && indentation_width(line) <= header_indent) {
			result.end = next == 0 ? 0 : next - 1;
			break;
		}
		next = next_end < source.size() ? next_end + 1 : source.size();
	}
	result.valid = true;
	return result;
}

TSNode first_kind_between(TSNode node, std::string_view kind, size_t begin, size_t end) {
	if (ts_node_is_null(node) || ts_node_end_byte(node) < begin || ts_node_start_byte(node) >= end) return {};
	if (node_type(node) == kind && ts_node_start_byte(node) >= begin && ts_node_end_byte(node) <= end) return node;
	for (uint32_t index = 0; index < ts_node_named_child_count(node); ++index) {
		auto found = first_kind_between(ts_node_named_child(node, index), kind, begin, end);
		if (!ts_node_is_null(found)) return found;
	}
	return {};
}

Symbol parameter_symbol(TSNode node, const Symbol &function, std::string_view source) {
	Symbol result;
	auto identifier = first_descendant(node, "identifier");
	if (ts_node_is_null(identifier)) identifier = first_descendant(node, "name");
	result.name = trim(node_text(identifier, source));
	auto start = syntax_position(source, ts_node_start_byte(identifier), ts_node_start_point(identifier));
	result.id = function.id + "::" + result.name + "@" + std::to_string(start.line) + ":" +
		std::to_string(start.character);
	result.qualified_name = result.id;
	result.uri = function.uri;
	result.kind = SymbolKind::Variable;
	result.range = node_range(node, source);
	result.selection_range = node_range(identifier, source);
	result.declared_type = trim(node_text(field(node, "type"), source));
	if (result.declared_type.empty()) {
		auto typed = first_descendant(node, "typed_parameter");
		result.declared_type = trim(node_text(field(typed, "type"), source));
	}
	result.is_inferred = is_inferred_type(result.declared_type);
	if (result.is_inferred) result.declared_type.clear();
	result.initializer = trim(node_text(field(node, "value"), source));
	result.is_local = true;
	result.is_parameter = true;
	result.is_variadic = node_type(node) == "variadic_parameter" || !ts_node_is_null(first_descendant(node, "variadic_parameter"));
	return result;
}

Symbol function_symbol(TSNode node, const std::string &uri, const std::string &owner_id, std::string_view source) {
	Symbol symbol;
	auto name_node = field(node, "name");
	symbol.name = node_type(node) == "constructor_definition" ? "_init" : trim(node_text(name_node, source));
	if (symbol.name.empty()) symbol.name = "<lambda>";
	symbol.id = owner_id + "::" + symbol.name;
	symbol.qualified_name = symbol.id;
	symbol.uri = uri;
	symbol.kind = symbol.name == "_init" ? SymbolKind::Constructor : SymbolKind::Method;
	symbol.range = node_range(node, source);
	symbol.selection_range = ts_node_is_null(name_node) ? symbol.range : node_range(name_node, source);
	symbol.declared_type = trim(node_text(field(node, "return_type"), source));
	symbol.is_static = has_named_child(node, "static_keyword");
	symbol.detail = (symbol.is_static ? "static func " : "func ") + symbol.name + "(";
	auto parameters = field(node, "parameters");
	for (uint32_t i = 0; i < ts_node_named_child_count(parameters); ++i) {
		auto parameter = parameter_symbol(ts_node_named_child(parameters, i), symbol, source);
		if (parameter.name.empty()) continue;
		if (!symbol.children.empty()) symbol.detail += ", ";
		symbol.detail += parameter.name;
		if (!parameter.declared_type.empty()) symbol.detail += ": " + parameter.declared_type;
		symbol.children.push_back(std::move(parameter));
	}
	symbol.detail += ") -> " + (symbol.declared_type.empty() ? "Variant" : symbol.declared_type);
	collect_locals(field(node, "body"), symbol, source);
	return symbol;
}

void add_parse_issue(std::vector<ParseIssue> &errors, Range range, std::string message) {
	for (auto &error : errors) {
		if (error.range.start == range.start && error.range.end == range.end) {
			if (error.message == "Syntax error.") error.message = std::move(message);
			return;
		}
	}
	errors.push_back({range, std::move(message)});
}

void collect_errors(TSNode node, std::string_view source, std::vector<ParseIssue> &errors) {
	if (ts_node_is_error(node) || ts_node_is_missing(node)) {
		add_parse_issue(errors, node_range(node, source), "Syntax error.");
	}

	auto type = node_type(node);
	bool malformed_declaration = false;
	if (type == "variable_statement" || type == "export_variable_statement" ||
			type == "onready_variable_statement" || type == "const_statement") {
		auto declaration = scan_declaration(node, source);
		malformed_declaration = declaration.malformed();
		if (declaration.missing_type || declaration.recovered_type) {
			auto byte = declaration.colon.value_or(declaration.end);
			add_parse_issue(errors, {byte_to_position(source, byte),
				byte_to_position(source, std::min(byte + 1, source.size()))}, R"(Expected type after ":".)");
		}
		if (declaration.missing_value || declaration.recovered_value) {
			auto byte = declaration.assignment.value_or(declaration.end);
			add_parse_issue(errors, {byte_to_position(source, byte),
				byte_to_position(source, std::min(byte + 1, source.size()))}, R"(Expected expression after "=".)");
		}
	}
	TSNode name{};
	std::string message;
	if (type == "variable_statement" || type == "export_variable_statement" || type == "onready_variable_statement") {
		name = field(node, "name");
		message = R"(Expected variable name after "var".)";
	} else if (type == "const_statement") {
		name = field(node, "name");
		message = R"(Expected constant name after "const".)";
	} else if (type == "function_definition") {
		name = field(node, "name");
		message = R"(Expected function name after "func".)";
	} else if (type == "signal_statement") {
		name = field(node, "name");
		message = R"(Expected signal name after "signal".)";
	} else if (type == "class_definition") {
		name = field(node, "name");
		message = R"(Expected identifier for the class name after "class".)";
	} else if (type == "class_name_statement") {
		name = field(node, "name");
		message = R"(Expected identifier for the global class name after "class_name".)";
	} else if (type == "enum_definition") {
		name = field(node, "name");
		message = R"(Expected identifier for the enum name after "enum".)";
	} else if (type == "for_statement") {
		name = field(node, "left");
		message = R"(Expected loop variable name after "for".)";
	} else if (type == "pattern_binding") {
		name = first_descendant(node, "identifier");
		if (ts_node_is_null(name)) name = first_descendant(node, "name");
		message = R"(Expected bind name after "var".)";
	} else if (type == "typed_parameter" || type == "default_parameter" ||
			type == "typed_default_parameter" || type == "variadic_parameter" || type == "parameter") {
		name = ts_node_named_child_count(node) ? ts_node_named_child(node, 0) : TSNode{};
		message = "Expected parameter name.";
	} else if (type == "enumerator") {
		name = field(node, "left");
		message = "Expected identifier for enum key.";
	}
	if (!ts_node_is_null(name)) {
		auto identifier = trim(node_text(name, source));
		if (is_gdscript_reserved_identifier(identifier)) {
			add_parse_issue(errors, node_range(name, source), std::move(message));
		}
	}
	if (malformed_declaration) return;
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		collect_errors(ts_node_named_child(node, i), source, errors);
	}
}

} // namespace

struct Document::Impl {
	TSParser *parser = nullptr;
	TSTree *tree = nullptr;
	~Impl() {
		if (tree) ts_tree_delete(tree);
		if (parser) ts_parser_delete(parser);
	}
};

Document::Document(std::string uri, std::string resource_path, std::string source, int64_t version) :
		impl_(std::make_unique<Impl>()), uri_(std::move(uri)), resource_path_(std::move(resource_path)),
		source_(std::move(source)), version_(version) {
	parse();
}

Document::~Document() = default;
Document::Document(Document &&) noexcept = default;
Document &Document::operator=(Document &&) noexcept = default;

void Document::parse() {
	impl_->parser = ts_parser_new();
	if (!ts_parser_set_language(impl_->parser, tree_sitter_gdscript())) return;
	impl_->tree = ts_parser_parse_string(impl_->parser, nullptr, source_.data(), static_cast<uint32_t>(source_.size()));
	if (!impl_->tree) return;
	auto root = ts_tree_root_node(impl_->tree);
	syntax_root_ = syntax_node(root, source_);
	collect_errors(root, source_, syntax_errors_);

	ClassRecord root_class;
	root_class.symbol.id = resource_path_;
	root_class.symbol.name = resource_path_.substr(resource_path_.find_last_of('/') + 1);
	if (root_class.symbol.name.ends_with(".gd")) root_class.symbol.name.resize(root_class.symbol.name.size() - 3);
	root_class.symbol.qualified_name = root_class.symbol.id;
	root_class.symbol.uri = uri_;
	root_class.symbol.kind = SymbolKind::Class;
	root_class.symbol.range = {{0, 0}, byte_to_position(source_, source_.size())};
	root_class.symbol.selection_range = {{0, 0}, {0, 0}};

	std::function<void(TSNode, ClassRecord &, const std::string &)> collect_class;
	collect_class = [&](TSNode container, ClassRecord &record, const std::string &owner_id) {
		for (uint32_t i = 0; i < ts_node_named_child_count(container); ++i) {
			auto child = ts_node_named_child(container, i);
			auto type = node_type(child);
			if (type == "class_name_statement") {
				auto name_node = field(child, "name");
				record.global_name = trim(node_text(name_node, source_));
				record.symbol.name = record.global_name;
				record.symbol.selection_range = node_range(name_node, source_);
				auto ext = field(child, "extends");
				if (!ts_node_is_null(ext)) record.extends_text = extends_text(ext, source_);
			} else if (type == "extends_statement") {
				record.extends_text = extends_text(child, source_);
			} else if (type == "variable_statement" || type == "export_variable_statement" ||
					type == "onready_variable_statement" || type == "const_statement") {
				record.members.push_back(variable_symbol(child, uri_, owner_id, source_, false));
			} else if (type == "function_definition" || type == "constructor_definition") {
				record.members.push_back(function_symbol(child, uri_, owner_id, source_));
			} else if (type == "signal_statement" || type == "enum_definition") {
				auto name_node = field(child, "name");
				Symbol symbol;
				symbol.name = trim(node_text(name_node, source_));
				symbol.id = owner_id + "::" + symbol.name;
				symbol.qualified_name = symbol.id;
				symbol.uri = uri_;
				symbol.kind = type == "signal_statement" ? SymbolKind::Event : SymbolKind::Enum;
				symbol.range = node_range(child, source_);
				symbol.selection_range = node_range(name_node, source_);
				symbol.declared_type = type == "signal_statement" ? "Signal" : symbol.name;
				symbol.detail = trim(node_text(child, source_));
				if (type == "signal_statement") {
					auto parameters = field(child, "parameters");
					if (!ts_node_is_null(parameters)) {
						for (uint32_t parameter_index = 0;
								parameter_index < ts_node_named_child_count(parameters); ++parameter_index) {
							auto parameter = parameter_symbol(ts_node_named_child(parameters, parameter_index), symbol, source_);
							if (!parameter.name.empty()) symbol.children.push_back(std::move(parameter));
						}
					}
				}
				if (type == "enum_definition") {
					auto body = field(child, "body");
					for (uint32_t value_index = 0; value_index < ts_node_named_child_count(body); ++value_index) {
						auto enumerator = ts_node_named_child(body, value_index);
						auto value_node = field(enumerator, "left");
						Symbol value;
						value.name = trim(node_text(value_node, source_));
						value.id = owner_id + "::" + (symbol.name.empty() ? "" : symbol.name + ".") + value.name;
						value.qualified_name = value.id;
						value.uri = uri_;
						value.kind = SymbolKind::Constant;
						value.range = node_range(enumerator, source_);
						value.selection_range = node_range(value_node, source_);
						value.declared_type = "int";
						if (symbol.name.empty()) record.members.push_back(std::move(value));
						else symbol.children.push_back(std::move(value));
					}
				}
				if (!symbol.name.empty()) record.members.push_back(std::move(symbol));
			} else if (type == "class_definition") {
				auto name_node = field(child, "name");
				ClassRecord inner;
				inner.symbol.name = trim(node_text(name_node, source_));
				inner.symbol.id = owner_id + "." + inner.symbol.name;
				inner.symbol.qualified_name = inner.symbol.id;
				inner.symbol.uri = uri_;
				inner.symbol.kind = SymbolKind::Class;
				inner.symbol.range = node_range(child, source_);
				inner.symbol.selection_range = node_range(name_node, source_);
				inner.extends_text = extends_text(field(child, "extends"), source_);
				if (inner.extends_text.empty()) inner.extends_text = "RefCounted";
				collect_class(field(child, "body"), inner, inner.symbol.id);
				Symbol inner_symbol = inner.symbol;
				inner_symbol.declared_type = inner.symbol.id;
				record.members.push_back(std::move(inner_symbol));
				classes_.push_back(std::move(inner));
			}
		}
	};
	collect_class(root, root_class, root_class.symbol.id);
	classes_.insert(classes_.begin(), std::move(root_class));

	// A parser error at the caret can shorten a function node to the last valid
	// token, or replace the whole function with ERROR. Recover the lexical block
	// so symbols declared before the error remain usable while editing.
	auto code = source_code_mask(source_);
	for (size_t line = 0; line < source_.size();) {
		auto extent = function_extent(source_, code, line);
		if (extent.valid) {
			auto header_position = byte_to_position(source_, extent.keyword);
			auto name_position = byte_to_position(source_, extent.name_start);
			auto name = std::string(source_.substr(extent.name_start, extent.name_end - extent.name_start));
			ClassRecord *owner = classes_.empty() ? nullptr : &classes_.front();
			for (auto &candidate : classes_) {
				if (!candidate.symbol.range.contains(header_position)) continue;
				if (!owner || candidate.symbol.range.start >= owner->symbol.range.start) owner = &candidate;
			}
			Symbol *existing = nullptr;
			if (owner) for (auto &member : owner->members) {
				if ((member.kind == SymbolKind::Method || member.kind == SymbolKind::Constructor) &&
						member.name == name && (member.selection_range.start == name_position ||
							(member.kind == SymbolKind::Constructor && member.range.contains(header_position)))) {
					existing = &member;
					break;
				}
			}
			if (existing) {
				existing->range.end = byte_to_position(source_, extent.end);
			} else if (owner) {
				Symbol recovered;
				recovered.name = name;
				recovered.id = owner->symbol.id + "::" + name;
				recovered.qualified_name = recovered.id;
				recovered.uri = uri_;
				recovered.kind = name == "_init" ? SymbolKind::Constructor : SymbolKind::Method;
				recovered.range = {header_position, byte_to_position(source_, extent.end)};
				recovered.selection_range = {name_position, byte_to_position(source_, extent.name_end)};
				recovered.is_static = extent.is_static;
				recovered.malformed = true;
				recovered.detail = (extent.is_static ? "static func " : "func ") + name + "(";
				auto parameters = first_kind_between(root, "parameters", extent.keyword, extent.header_end);
				for (uint32_t index = 0; index < ts_node_named_child_count(parameters); ++index) {
					auto parameter = parameter_symbol(ts_node_named_child(parameters, index), recovered, source_);
					if (parameter.name.empty()) continue;
					if (!recovered.children.empty()) recovered.detail += ", ";
					recovered.detail += parameter.name;
					if (!parameter.declared_type.empty()) recovered.detail += ": " + parameter.declared_type;
					recovered.children.push_back(std::move(parameter));
				}
				recovered.detail += ") -> Variant";
				collect_locals(root, recovered, source_, extent.body_start,
					extent.end == source_.size() ? extent.end : extent.end + 1);
				owner->members.push_back(std::move(recovered));
			}
		}
		auto end = line_end(source_, line);
		if (end == source_.size()) break;
		line = end + 1;
	}
}

std::string_view Document::text(const SyntaxNode &node) const {
	if (node.start_byte > source_.size() || node.end_byte > source_.size() || node.start_byte > node.end_byte) return {};
	return std::string_view(source_).substr(node.start_byte, node.end_byte - node.start_byte);
}

const ClassRecord *Document::class_at(Position position) const {
	const ClassRecord *best = classes_.empty() ? nullptr : &classes_.front();
	for (const auto &record : classes_) {
		if (record.symbol.range.contains(position) && (!best || record.symbol.range.start >= best->symbol.range.start)) {
			best = &record;
		}
	}
	return best;
}

const Symbol *Document::symbol_at(Position position) const {
	const Symbol *best = nullptr;
	std::function<void(const Symbol &)> visit = [&](const Symbol &symbol) {
		if (!symbol.range.contains(position)) return;
		if (!best || symbol.range.start >= best->range.start) best = &symbol;
		for (const auto &child : symbol.children) visit(child);
	};
	for (const auto &record : classes_) {
		visit(record.symbol);
		for (const auto &member : record.members) visit(member);
	}
	return best;
}

std::vector<const Symbol *> Document::locals_at(Position position) const {
	std::vector<const Symbol *> result;
	for (const auto &record : classes_) {
		if (!record.symbol.range.contains(position)) continue;
		for (const auto &member : record.members) {
			if ((member.kind != SymbolKind::Method && member.kind != SymbolKind::Constructor) || !member.range.contains(position)) continue;
			for (const auto &local : member.children) {
				if (local.range.start <= position) result.push_back(&local);
			}
		}
	}
	return result;
}

const Symbol *Document::find_local(std::string_view name, Position position) const {
	const Symbol *best = nullptr;
	for (auto *local : locals_at(position)) {
		if (local->name == name && (!best || local->range.start >= best->range.start)) best = local;
	}
	return best;
}

} // namespace gdscript_lsp
