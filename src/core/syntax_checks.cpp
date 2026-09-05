#include "core/syntax_checks.hpp"
#include "core/text.hpp"

#include <functional>

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
}

std::vector<ParseIssue> structural_issues(const Document &document) {
	std::vector<ParseIssue> result;
	std::function<void(const SyntaxNode &, unsigned)> visit = [&](const SyntaxNode &node, unsigned loops) {
		auto add = [&](std::string message, Range range) { result.push_back({range, std::move(message)}); };
		if (node.kind == "function_definition" || node.kind == "constructor_definition" || node.kind == "lambda") {
			loops = 0;
			if (node.kind == "function_definition" && !field(node, "name")) {
				add("Standalone lambdas cannot be accessed; assign the lambda or pass it as an argument.",
					{node.range.start, byte_to_position(document.source(), node.start_byte + 4)});
			}
		}
		if ((node.kind == "break_statement" || node.kind == "continue_statement") && !loops) {
			add("Cannot use \"" + std::string(node.kind == "break_statement" ? "break" : "continue") + "\" outside of a loop.", node.range);
		}
		if (node.kind == "parameters") {
			bool optional = false;
			for (const auto &parameter : node.children) {
				if (parameter.kind == "comment" || parameter.has_error) continue;
				if (field(parameter, "value")) optional = true;
				else if (optional && parameter.kind != "variadic_parameter") add("Required parameters cannot follow parameters with default values.", parameter.range);
			}
		}
		if ((node.field == "type" || node.field == "return_type") && has_void(document.text(node)) &&
			!(node.field == "return_type" && trim(document.text(node)) == "void")) {
			add("The type \"void\" is only allowed as a function return type.", node.range);
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
			visit(child, child_loops);
		}
	};
	visit(document.syntax_root(), 0);
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
