#include "core/semantic_analyzer.hpp"

#include "core/document.hpp"
#include "core/text.hpp"
#include "core/workspace.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace gdscript_lsp {
namespace {

const SyntaxNode *field(const SyntaxNode &node, std::string_view name) {
	for (const auto &child : node.children) if (child.field == name) return &child;
	return nullptr;
}

const SyntaxNode *child_kind(const SyntaxNode &node, std::string_view kind) {
	for (const auto &child : node.children) if (child.kind == kind) return &child;
	return nullptr;
}

std::string text(const Document &document, const SyntaxNode &node) {
	return trim(document.text(node));
}

const SyntaxNode *first_identifier(const SyntaxNode &node) {
	if (node.kind == "identifier" || node.kind == "name") return &node;
	for (const auto &child : node.children) {
		if (auto *found = first_identifier(child)) return found;
	}
	return nullptr;
}

struct Value {
	ResolvedType type;
	std::vector<CallableSignature> signatures;
	bool resolved = false;
	bool callable = false;
};

using Scope = std::unordered_map<std::string, Value>;

} // namespace

class SemanticAnalyzerImpl {
public:
	SemanticAnalyzerImpl(const Workspace &p_workspace, const Document &p_document) : workspace(p_workspace), document(p_document) {}

	std::vector<Diagnostic> run() {
		analyze_class_container(document.syntax_root());
		return std::move(diagnostics);
	}

private:
	const Workspace &workspace;
	const Document &document;
	const ClassRecord *current_class = nullptr;
	std::vector<Scope> scopes;
	std::vector<Diagnostic> diagnostics;

	void add(std::string code, std::string message, Range range) {
		diagnostics.emplace_back(std::move(code), std::move(message), range);
	}

	CallableSignature script_signature(const Symbol &function) const {
		CallableSignature result;
		result.return_type = function.declared_type.empty() ? "Variant" : function.declared_type;
		for (const auto &child : function.children) {
			if (!child.is_parameter) continue;
			if (child.is_variadic) {
				result.is_vararg = true;
				continue;
			}
			result.arguments.push_back({child.name, child.declared_type.empty() ? "Variant" : child.declared_type,
				!child.initializer.empty()});
		}
		return result;
	}

	CallableSignature syntax_signature(const SyntaxNode &function) const {
		CallableSignature result;
		if (auto *return_type = field(function, "return_type")) result.return_type = text(document, *return_type);
		else result.return_type = "Variant";
		if (auto *parameters = field(function, "parameters")) for (const auto &parameter : parameters->children) {
			if (parameter.kind == "variadic_parameter") {
				result.is_vararg = true;
				continue;
			}
			auto *identifier = first_identifier(parameter);
			if (!identifier) continue;
			auto *type_node = field(parameter, "type");
			result.arguments.push_back({text(document, *identifier), type_node ? text(document, *type_node) : "Variant",
				field(parameter, "value") != nullptr});
		}
		return result;
	}

	Value symbol_value(const Symbol &symbol, Position position) const {
		Value result;
		result.resolved = true;
		if (symbol.kind == SymbolKind::Method || symbol.kind == SymbolKind::Constructor) {
			result.type = {TypeKind::Callable, "Callable", symbol.id};
			result.callable = true;
			result.signatures.push_back(script_signature(symbol));
			return result;
		}
		std::vector<std::string> stack;
		result.type = workspace.type_of_symbol(symbol, document, position, stack);
		return result;
	}

	Value resolve_name(std::string_view name, Position position, bool call_target, Range range) {
		for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
			if (auto found = scope->find(std::string(name)); found != scope->end()) return found->second;
		}
		if (name == "self" && current_class) {
			return {{TypeKind::ScriptClass, current_class->symbol.name, current_class->symbol.id, true}, {}, true, false};
		}
		if (name == "super" && current_class && !current_class->base_class_id.empty()) {
			if (current_class->base_class_id.starts_with("native:")) {
				return {{TypeKind::NativeClass, current_class->base_class_id.substr(7), current_class->base_class_id, true}, {}, true, false};
			}
			auto *base = workspace.find_class(current_class->base_class_id);
			if (base) return {{TypeKind::ScriptClass, base->symbol.name, base->symbol.id, true}, {}, true, false};
		}
		if (name == "new" && current_class) {
			Value result{{TypeKind::Callable, "Callable"}, {}, true, true};
			auto *constructor = workspace.find_member(*current_class, "_init");
			if (constructor) result.signatures.push_back(script_signature(*constructor));
			else result.signatures.push_back({});
			for (auto &signature : result.signatures) signature.return_type = current_class->symbol.id;
			return result;
		}
		if (current_class) {
			if (auto *member = workspace.find_member(*current_class, name)) return symbol_value(*member, position);
			auto native = workspace.native_base(*current_class);
			if (!native.empty()) {
				if (auto *member = workspace.native_api_.find_member(native, name)) return native_member_value(*member);
			}
		}
		if (auto found = workspace.autoloads_.find(std::string(name)); found != workspace.autoloads_.end()) {
			auto type = workspace.type_from_name(workspace.resolve_path_reference(found->second, document.resource_path()), current_class);
			type.instance = true;
			return {type, {}, true, false};
		}
		if (auto singleton = workspace.native_api_.singleton_type(name)) {
			return {{TypeKind::NativeClass, *singleton, "native:" + *singleton, true}, {}, true, false};
		}
		if (auto *utility = workspace.native_api_.find_utility_function(name)) {
			return {{TypeKind::Callable, "Callable"}, {*utility}, true, true};
		}
		if (name == "range") {
			return {{TypeKind::Callable, "Callable"}, {
				{"Array", {{"end", "int", false}}, false},
				{"Array", {{"start", "int", false}, {"end", "int", false}}, false},
				{"Array", {{"start", "int", false}, {"end", "int", false}, {"step", "int", false}}, false},
			}, true, true};
		}
		if (name == "assert") {
			return {{TypeKind::Callable, "Callable"}, {{"void", {{"condition", "bool", false}, {"message", "String", true}}, false}}, true, true};
		}
		if (name == "load" || name == "preload") {
			return {{TypeKind::Callable, "Callable"}, {{"Variant", {{"path", "String", false}}, false}}, true, true};
		}
		if (workspace.native_api_.is_global_enum(name)) {
			return {{TypeKind::Enum, std::string(name), "global:" + std::string(name), false}, {}, true, false};
		}
		if (auto enumeration = workspace.native_api_.global_enum_for_value(name)) {
			return {{TypeKind::Enum, *enumeration, "global:" + *enumeration, false}, {}, true, false};
		}
		if (workspace.native_api_.has_global_symbol(name) || name == "PI" || name == "TAU" || name == "INF" || name == "NAN") {
			return {{TypeKind::Builtin, "float"}, {}, true, false};
		}
		auto type = workspace.type_from_name(std::string(name), current_class);
		if (type.known()) {
			type.instance = false;
			Value result{type, {}, true, false};
			if (type.kind == TypeKind::Builtin) {
				if (auto *constructors = workspace.native_api_.constructors(type.name)) {
					result.signatures = *constructors;
					for (auto &signature : result.signatures) signature.return_type = type.name;
					result.callable = !result.signatures.empty();
				}
			}
			return result;
		}
		add(call_target ? "undefined-function" : "undefined-identifier",
			(call_target ? "Function \"" : "Identifier \"") + std::string(name) +
			(call_target ? "()\" is not declared in the current scope." : "\" is not declared in the current scope."), range);
		return {};
	}

	Value native_member_value(const NativeMember &member) const {
		Value result;
		result.resolved = true;
		if (member.kind == SymbolKind::Enum) {
			result.type = {TypeKind::Enum, member.name, "nativeenum:" + member.owner + "." + member.name, false};
		} else if (member.kind == SymbolKind::Event) {
			result.type = {TypeKind::Signal, "Signal"};
		} else if (member.signature) {
			result.type = {TypeKind::Callable, "Callable"};
			result.signatures.push_back(*member.signature);
			result.callable = true;
		} else {
			result.type = member.kind == SymbolKind::Constant && member.type == "void" ?
				ResolvedType{TypeKind::Builtin, "int"} : workspace.type_from_name(member.type, current_class);
			if (!result.type.known()) result.type = {TypeKind::Variant, "Variant"};
		}
		return result;
	}

	Value member_value(const Value &receiver, std::string_view name, Range range) {
		if (!receiver.resolved || !receiver.type.known() || receiver.type.kind == TypeKind::Variant ||
			(receiver.type.kind == TypeKind::Builtin && receiver.type.name == "Dictionary")) return {};
		if (name == "new" && !receiver.type.instance &&
			(receiver.type.kind == TypeKind::ScriptClass || receiver.type.kind == TypeKind::NativeClass)) {
			Value result{{TypeKind::Callable, "Callable"}, {}, true, true};
			if (receiver.type.kind == TypeKind::ScriptClass) {
				auto *record = workspace.find_class(receiver.type.symbol_id);
				auto *constructor = record ? workspace.find_member(*record, "_init") : nullptr;
				if (constructor) result.signatures.push_back(script_signature(*constructor));
				else result.signatures.push_back({});
			} else if (auto *constructors = workspace.native_api_.constructors(receiver.type.name)) {
				result.signatures = *constructors;
				if (result.signatures.empty()) result.signatures.push_back({});
			} else {
				result.signatures.push_back({});
			}
			for (auto &signature : result.signatures) signature.return_type = receiver.type.name;
			return result;
		}
		if (receiver.type.kind == TypeKind::ScriptClass) {
			if (auto *record = workspace.find_class(receiver.type.symbol_id)) {
				if (auto *member = workspace.find_member(*record, name)) return symbol_value(*member, range.start);
				auto native = workspace.native_base(*record);
				if (!native.empty()) if (auto *member = workspace.native_api_.find_member(native, name)) return native_member_value(*member);
			}
		} else if (receiver.type.kind == TypeKind::NativeClass || receiver.type.kind == TypeKind::Builtin ||
			receiver.type.kind == TypeKind::Callable || receiver.type.kind == TypeKind::Signal) {
			if (auto *member = workspace.native_api_.find_member(receiver.type.name, name)) return native_member_value(*member);
		} else if (receiver.type.kind == TypeKind::Enum) {
			if (receiver.type.symbol_id.starts_with("global:") && workspace.native_api_.global_enum_has_value(
					receiver.type.symbol_id.substr(7), name)) {
				return {{TypeKind::Builtin, "int"}, {}, true, false};
			}
			if (receiver.type.symbol_id.starts_with("nativeenum:")) {
				auto qualified = receiver.type.symbol_id.substr(11);
				auto separator = qualified.rfind('.');
				if (separator != std::string::npos && workspace.native_api_.enum_has_value(
						qualified.substr(0, separator), qualified.substr(separator + 1), name)) {
					return {{TypeKind::Builtin, "int"}, {}, true, false};
				}
			}
			for (const auto &[id, record] : workspace.classes_) {
				(void)id;
				for (const auto &member : record->members) if (member.id == receiver.type.symbol_id) {
					for (const auto &value : member.children) if (value.name == name) return symbol_value(value, range.start);
				}
			}
		}
		add("unknown-member", "Member \"" + std::string(name) + "\" does not exist on type \"" +
			receiver.type.display() + "\".", range);
		return {};
	}

	std::vector<const SyntaxNode *> argument_nodes(const SyntaxNode *arguments) const {
		std::vector<const SyntaxNode *> result;
		if (!arguments) return result;
		for (const auto &child : arguments->children) result.push_back(&child);
		return result;
	}

	Value call_value(Value callee, const SyntaxNode *arguments, Range range) {
		auto nodes = argument_nodes(arguments);
		std::vector<Value> values;
		for (auto *node : nodes) values.push_back(evaluate(*node));
		if (!callee.resolved) return {};
		if (!callee.callable) {
			if (callee.type.kind != TypeKind::Variant && callee.type.known()) {
				add("not-callable", "A value of type \"" + callee.type.display() + "\" is not callable.", range);
			}
			return {};
		}
		std::vector<const CallableSignature *> arity_matches;
		for (const auto &signature : callee.signatures) {
			size_t minimum = 0;
			for (const auto &argument : signature.arguments) if (!argument.has_default) ++minimum;
			if (!signature.arity_known) minimum = 0;
			auto maximum = signature.is_vararg || !signature.arity_known ?
				std::numeric_limits<size_t>::max() : signature.arguments.size();
			if (nodes.size() >= minimum && nodes.size() <= maximum) arity_matches.push_back(&signature);
		}
		if (arity_matches.empty()) {
			add("argument-count", "No callable overload accepts " + std::to_string(nodes.size()) + " argument(s).", range);
			return {};
		}
		for (auto *signature : arity_matches) {
			bool compatible = true;
			for (size_t index = 0; index < values.size() && index < signature->arguments.size(); ++index) {
				auto expected = workspace.type_from_name(signature->arguments[index].type, current_class);
				if (expected.known() && values[index].type.known() && !workspace.is_assignable(expected, values[index].type)) {
					compatible = false;
					break;
				}
			}
			if (compatible) {
				auto result_type = workspace.type_from_name(signature->return_type, current_class);
				if (!result_type.known()) result_type = {TypeKind::Variant, "Variant"};
				return {result_type, {}, true, false};
			}
		}
		auto *signature = arity_matches.front();
		for (size_t index = 0; index < values.size() && index < signature->arguments.size(); ++index) {
			auto expected = workspace.type_from_name(signature->arguments[index].type, current_class);
			if (expected.known() && values[index].type.known() && !workspace.is_assignable(expected, values[index].type)) {
				add("argument-type", "Argument " + std::to_string(index + 1) + " expects \"" + expected.display() +
					"\", but received \"" + values[index].type.display() + "\".", nodes[index]->range);
				break;
			}
		}
		auto result_type = workspace.type_from_name(signature->return_type, current_class);
		return {result_type, {}, true, false};
	}

	Value evaluate(const SyntaxNode &node, bool call_target = false) {
		if (node.kind == "identifier" || node.kind == "name") return resolve_name(text(document, node), node.range.start, call_target, node.range);
		if (node.kind == "integer") return {{TypeKind::Builtin, "int"}, {}, true, false};
		if (node.kind == "float") return {{TypeKind::Builtin, "float"}, {}, true, false};
		if (node.kind == "string") return {{TypeKind::Builtin, "String"}, {}, true, false};
		if (node.kind == "string_name") return {{TypeKind::Builtin, "StringName"}, {}, true, false};
		if (node.kind == "node_path") return {{TypeKind::Builtin, "NodePath"}, {}, true, false};
		if (node.kind == "true" || node.kind == "false") return {{TypeKind::Builtin, "bool"}, {}, true, false};
		if (node.kind == "null") return {{TypeKind::Variant, "Variant"}, {}, true, false};
		if (node.kind == "get_node") return {{TypeKind::Variant, "Variant"}, {}, true, false};
		if (node.kind == "array") {
			for (const auto &child : node.children) evaluate(child);
			return {{TypeKind::Builtin, "Array"}, {}, true, false};
		}
		if (node.kind == "dictionary") {
			for (const auto &child : node.children) evaluate(child);
			return {{TypeKind::Builtin, "Dictionary"}, {}, true, false};
		}
		if (node.kind == "pair") {
			for (const auto &child : node.children) evaluate(child);
			return {{TypeKind::Variant, "Variant"}, {}, true, false};
		}
		if (node.kind == "parenthesized_expression" || node.kind == "await_expression" || node.kind == "unary_operator") {
			return node.children.empty() ? Value{} : evaluate(node.children.front());
		}
		if (node.kind == "binary_operator") {
			auto *left = field(node, "left");
			auto *right = field(node, "right");
			auto left_value = left ? evaluate(*left) : Value{};
			auto right_value = right ? evaluate(*right) : Value{};
			auto source = text(document, node);
			if (source.find(" == ") != std::string::npos || source.find(" != ") != std::string::npos ||
				source.find(" < ") != std::string::npos || source.find(" > ") != std::string::npos ||
				source.find(" is ") != std::string::npos || source.find(" in ") != std::string::npos) {
				return {{TypeKind::Builtin, "bool"}, {}, true, false};
			}
			if (left_value.type.known() && right_value.type.known() && left_value.type.name == right_value.type.name) return left_value;
			return {{TypeKind::Variant, "Variant"}, {}, true, false};
		}
		if (node.kind == "conditional_expression") {
			if (auto *condition = field(node, "condition")) evaluate(*condition);
			auto left = field(node, "left") ? evaluate(*field(node, "left")) : Value{};
			auto right = field(node, "right") ? evaluate(*field(node, "right")) : Value{};
			return left.type.known() && right.type.known() && left.type.name == right.type.name ? left :
				Value{{TypeKind::Variant, "Variant"}, {}, true, false};
		}
		if (node.kind == "assignment" || node.kind == "augmented_assignment") {
			if (auto *left = field(node, "left")) evaluate(*left);
			return field(node, "right") ? evaluate(*field(node, "right")) : Value{};
		}
		if (node.kind == "subscript") {
			auto base = node.children.empty() ? Value{} : evaluate(node.children.front());
			if (auto *arguments = field(node, "arguments")) for (const auto &child : arguments->children) evaluate(child);
			if (base.type.kind == TypeKind::Builtin && base.type.name == "Array" && !base.type.arguments.empty()) {
				return {base.type.arguments.front(), {}, true, false};
			}
			if (base.type.kind == TypeKind::Builtin && base.type.name == "String") return {{TypeKind::Builtin, "String"}, {}, true, false};
			return {{TypeKind::Variant, "Variant"}, {}, true, false};
		}
		if (node.kind == "call") {
			const SyntaxNode *callee_node = nullptr;
			for (const auto &child : node.children) if (child.field != "arguments") { callee_node = &child; break; }
			auto callee = callee_node ? evaluate(*callee_node, callee_node->kind == "identifier") : Value{};
			return call_value(std::move(callee), field(node, "arguments"), node.range);
		}
		if (node.kind == "base_call") {
			auto *identifier = first_identifier(node);
			Value callee;
			if (identifier && current_class && !current_class->base_class_id.empty()) {
				Value base;
				if (current_class->base_class_id.starts_with("native:")) base = {{TypeKind::NativeClass,
					current_class->base_class_id.substr(7), current_class->base_class_id, true}, {}, true, false};
				else if (auto *record = workspace.find_class(current_class->base_class_id)) base = {{TypeKind::ScriptClass,
					record->symbol.name, record->symbol.id, true}, {}, true, false};
				callee = member_value(base, text(document, *identifier), identifier->range);
			}
			return call_value(std::move(callee), field(node, "arguments"), node.range);
		}
		if (node.kind == "attribute") {
			if (node.children.empty()) return {};
			Value current = evaluate(node.children.front());
			for (size_t index = 1; index < node.children.size(); ++index) {
				const auto &part = node.children[index];
				if (part.kind == "identifier") current = member_value(current, text(document, part), part.range);
				else if (part.kind == "attribute_call") {
					auto *identifier = first_identifier(part);
					current = identifier ? call_value(member_value(current, text(document, *identifier), identifier->range),
						field(part, "arguments"), part.range) : Value{};
				} else if (part.kind == "attribute_subscript") {
					auto *identifier = first_identifier(part);
					current = identifier ? member_value(current, text(document, *identifier), identifier->range) : Value{};
					if (auto *arguments = field(part, "arguments")) for (const auto &child : arguments->children) evaluate(child);
					current = {{TypeKind::Variant, "Variant"}, {}, true, false};
				}
			}
			return current;
		}
		if (node.kind == "lambda") {
			analyze_lambda(node);
			return {{TypeKind::Callable, "Callable"}, {syntax_signature(node)}, true, true};
		}
		Value last;
		for (const auto &child : node.children) last = evaluate(child);
		return last;
	}

	const Symbol *function_symbol(const SyntaxNode &node) const {
		if (!current_class) return nullptr;
		auto *name_node = field(node, "name");
		auto name = name_node ? text(document, *name_node) : (node.kind == "constructor_definition" ? "_init" : "");
		for (const auto &member : current_class->members) {
			if (member.name == name && member.range.start == node.range.start) return &member;
		}
		return nullptr;
	}

	void bind_parameters(const SyntaxNode *parameters, const Symbol *function) {
		if (!parameters) return;
		for (const auto &parameter : parameters->children) {
			auto *identifier = first_identifier(parameter);
			if (!identifier) continue;
			if (auto *value = field(parameter, "value")) evaluate(*value);
			ResolvedType type{TypeKind::Variant, "Variant"};
			if (function) for (const auto &child : function->children) {
				if (child.is_parameter && child.name == text(document, *identifier)) {
					type = workspace.type_from_name(child.declared_type, current_class);
					if (!type.known()) type = {TypeKind::Variant, "Variant"};
					break;
				}
			} else if (auto *type_node = field(parameter, "type")) {
				type = workspace.type_from_name(text(document, *type_node), current_class);
				if (!type.known()) {
					add("unknown-type", "Could not find type \"" + text(document, *type_node) + "\" in the current scope.",
						type_node->range);
					type = {TypeKind::Variant, "Variant"};
				}
			}
			scopes.back()[text(document, *identifier)] = {type, {}, true, false};
		}
	}

	void analyze_function(const SyntaxNode &node) {
		auto *saved_class = current_class;
		current_class = document.class_at(node.range.start);
		auto *function = function_symbol(node);
		auto *body = field(node, "body");
		if (!body) { current_class = saved_class; return; }
		scopes.emplace_back();
		bind_parameters(field(node, "parameters"), function);
		auto expected = function && !function->declared_type.empty() ? workspace.type_from_name(function->declared_type, current_class) : ResolvedType{};
		analyze_block(*body, expected);
		if (function && expected.known() && expected.kind != TypeKind::Void && !always_returns(*body)) {
			add("missing-return-path", "Not all code paths return a value of type \"" + expected.display() + "\".", function->selection_range);
		}
		scopes.pop_back();
		current_class = saved_class;
	}

	void analyze_lambda(const SyntaxNode &node) {
		auto *body = field(node, "body");
		if (!body) return;
		scopes.emplace_back();
		bind_parameters(field(node, "parameters"), nullptr);
		ResolvedType expected;
		if (auto *type = field(node, "return_type")) expected = workspace.type_from_name(text(document, *type), current_class);
		analyze_block(*body, expected);
		if (expected.known() && expected.kind != TypeKind::Void && !always_returns(*body)) {
			add("missing-return-path", "Not all lambda code paths return a value of type \"" + expected.display() + "\".", node.range);
		}
		scopes.pop_back();
	}

	void analyze_accessor(const SyntaxNode &node, const ResolvedType &expected) {
		auto *body = field(node, "body");
		if (!body) return;
		scopes.emplace_back();
		bind_parameters(child_kind(node, "parameters"), nullptr);
		analyze_block(*body, expected);
		if (expected.known() && expected.kind != TypeKind::Void && !always_returns(*body)) {
			add("missing-return-path", "Not all accessor code paths return a value of type \"" + expected.display() + "\".", node.range);
		}
		scopes.pop_back();
	}

	void bind_local(const SyntaxNode &node, Value initializer) {
		if (scopes.empty()) return;
		auto *name_node = field(node, "name");
		if (!name_node) return;
		ResolvedType type{TypeKind::Variant, "Variant"};
		if (auto *type_node = field(node, "type"); type_node && text(document, *type_node) != ":=") {
			type = workspace.type_from_name(text(document, *type_node), current_class);
		} else if (initializer.type.known()) {
			type = initializer.type;
		}
		if (!type.known()) type = {TypeKind::Variant, "Variant"};
		scopes.back()[text(document, *name_node)] = {type, {}, true, false};
	}

	void check_declared_assignment(const SyntaxNode &node, const Value &initializer) {
		auto *type_node = field(node, "type");
		if (!type_node) return;
		auto declared = text(document, *type_node);
		if (declared.empty() || declared == ":=") return;
		auto expected = workspace.type_from_name(declared, current_class);
		if (!expected.known() || !initializer.type.known() || initializer.type.kind == TypeKind::Variant) return;
		if (!workspace.is_assignable(expected, initializer.type)) {
			add("type-mismatch", "Cannot assign a value of type \"" + initializer.type.display() +
				"\" to \"" + declared + "\".", node.range);
		}
	}

	void analyze_block(const SyntaxNode &block, const ResolvedType &expected_return) {
		for (const auto &statement : block.children) analyze_statement(statement, expected_return);
	}

	void analyze_scoped_body(const SyntaxNode *body, const ResolvedType &expected_return,
		std::optional<std::pair<std::string, Value>> binding = std::nullopt) {
		if (!body) return;
		scopes.emplace_back();
		if (binding) scopes.back()[binding->first] = binding->second;
		analyze_block(*body, expected_return);
		scopes.pop_back();
	}

	void analyze_statement(const SyntaxNode &node, const ResolvedType &expected_return) {
		if (node.kind == "variable_statement" || node.kind == "const_statement" ||
			node.kind == "export_variable_statement" || node.kind == "onready_variable_statement") {
			Value initializer;
			if (auto *value = field(node, "value")) initializer = evaluate(*value);
			check_declared_assignment(node, initializer);
			bind_local(node, initializer);
			return;
		}
		if (node.kind == "expression_statement") {
			for (const auto &child : node.children) evaluate(child);
			return;
		}
		if (node.kind == "return_statement") {
			const SyntaxNode *value_node = node.children.empty() ? nullptr : &node.children.front();
			if (!expected_return.known()) { if (value_node) evaluate(*value_node); return; }
			if (expected_return.kind == TypeKind::Void && value_node) {
				evaluate(*value_node);
				add("return-value-in-void", "A void function cannot return a value.", node.range);
			} else if (expected_return.kind != TypeKind::Void && !value_node) {
				add("missing-return-value", "A value of type \"" + expected_return.display() + "\" must be returned.", node.range);
			} else if (value_node) {
				auto actual = evaluate(*value_node).type;
				if (actual.known() && actual.kind != TypeKind::Variant && !workspace.is_assignable(expected_return, actual)) {
					add("return-type-mismatch", "Cannot return \"" + actual.display() + "\" from a function returning \"" +
						expected_return.display() + "\".", value_node->range);
				}
			}
			return;
		}
		if (node.kind == "if_statement") {
			if (auto *condition = field(node, "condition")) evaluate(*condition);
			analyze_scoped_body(field(node, "body"), expected_return);
			for (const auto &alternative : node.children) if (alternative.field == "alternative") {
				if (auto *condition = field(alternative, "condition")) evaluate(*condition);
				analyze_scoped_body(field(alternative, "body"), expected_return);
			}
			return;
		}
		if (node.kind == "for_statement") {
			if (auto *right = field(node, "right")) evaluate(*right);
			auto *left = field(node, "left");
			Value binding{{TypeKind::Variant, "Variant"}, {}, true, false};
			analyze_scoped_body(field(node, "body"), expected_return,
				left ? std::optional(std::pair{text(document, *left), binding}) : std::nullopt);
			return;
		}
		if (node.kind == "while_statement") {
			if (auto *condition = field(node, "condition")) evaluate(*condition);
			analyze_scoped_body(field(node, "body"), expected_return);
			return;
		}
		if (node.kind == "match_statement") {
			if (auto *value = field(node, "value")) evaluate(*value);
			auto *match_body = field(node, "body");
			if (match_body) for (const auto &section : match_body->children) if (section.kind == "pattern_section") {
				scopes.emplace_back();
				std::function<void(const SyntaxNode &)> bind_patterns = [&](const SyntaxNode &pattern) {
					if (pattern.kind == "pattern_binding") {
						if (auto *identifier = first_identifier(pattern)) scopes.back()[text(document, *identifier)] =
							{{TypeKind::Variant, "Variant"}, {}, true, false};
						return;
					}
					for (const auto &child : pattern.children) bind_patterns(child);
				};
				bind_patterns(section);
				if (auto *guard = child_kind(section, "pattern_guard")) for (const auto &child : guard->children) evaluate(child);
				if (auto *body = field(section, "body")) analyze_block(*body, expected_return);
				scopes.pop_back();
			}
			return;
		}
		if (node.kind == "function_definition" || node.kind == "constructor_definition") {
			analyze_function(node);
			return;
		}
		for (const auto &child : node.children) analyze_statement(child, expected_return);
	}

	bool always_returns(const SyntaxNode &node) const {
		if (node.kind == "return_statement") return true;
		if (node.kind == "body") {
			for (const auto &statement : node.children) if (always_returns(statement)) return true;
			return false;
		}
		if (node.kind == "if_statement") {
			auto *body = field(node, "body");
			if (!body || !always_returns(*body)) return false;
			bool has_else = false;
			for (const auto &alternative : node.children) if (alternative.field == "alternative") {
				has_else = has_else || alternative.kind == "else_clause";
				auto *alternative_body = field(alternative, "body");
				if (!alternative_body || !always_returns(*alternative_body)) return false;
			}
			return has_else;
		}
		if (node.kind == "match_statement") {
			auto *body = field(node, "body");
			if (!body || body->children.empty()) return false;
			bool wildcard = false;
			for (const auto &section : body->children) if (section.kind == "pattern_section") {
				auto source = text(document, section);
				wildcard = wildcard || source.starts_with("_") || source.find(", _") != std::string::npos;
				auto *section_body = field(section, "body");
				if (!section_body || !always_returns(*section_body)) return false;
			}
			return wildcard;
		}
		return false;
	}

	void analyze_class_container(const SyntaxNode &container) {
		for (const auto &node : container.children) {
			if (node.kind == "function_definition" || node.kind == "constructor_definition") analyze_function(node);
			else if (node.kind == "class_definition") {
				if (auto *body = field(node, "body")) analyze_class_container(*body);
			} else if (node.kind == "variable_statement" || node.kind == "const_statement" ||
				node.kind == "export_variable_statement" || node.kind == "onready_variable_statement") {
				auto *saved = current_class;
				current_class = document.class_at(node.range.start);
				Value initializer;
				if (auto *value = field(node, "value")) initializer = evaluate(*value);
				check_declared_assignment(node, initializer);
				ResolvedType property_type;
				if (auto *type_node = field(node, "type"); type_node && text(document, *type_node) != ":=") {
					property_type = workspace.type_from_name(text(document, *type_node), current_class);
				}
				std::function<void(const SyntaxNode &)> accessors = [&](const SyntaxNode &candidate) {
					if (candidate.kind == "set_body") analyze_accessor(candidate, {TypeKind::Void, "void"});
					else if (candidate.kind == "get_body") analyze_accessor(candidate, property_type);
					else for (const auto &child : candidate.children) accessors(child);
				};
				accessors(node);
				current_class = saved;
			}
		}
	}
};

std::vector<Diagnostic> SemanticAnalyzer::run(const Workspace &workspace, const Document &document) {
	return SemanticAnalyzerImpl(workspace, document).run();
}

} // namespace gdscript_lsp
