#include "core/semantic_analyzer.hpp"
#include "core/syntax_checks.hpp"
#include "core/warnings.hpp"

#include "core/document.hpp"
#include "core/gdscript_api.hpp"
#include "core/text.hpp"
#include "core/workspace.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
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

struct Binding {
	std::string name;
	std::string warning;
	Range range;
	bool read = false;
	bool written = false;
};

struct Value {
	ResolvedType type;
	std::shared_ptr<Binding> binding;
	std::vector<Value> elements;
	bool array_literal = false;
	std::vector<CallableSignature> signatures;
	bool resolved = false;
	bool callable = false;
	// Script signature type names are relative to the class that declares the
	// callable, not the class containing the call site.
	std::string callable_context_id;
	Value() = default;
	Value(ResolvedType p_type, std::vector<CallableSignature> p_signatures,
			bool p_resolved, bool p_callable, std::string p_callable_context_id = {}) :
			type(std::move(p_type)), signatures(std::move(p_signatures)), resolved(p_resolved),
			callable(p_callable), callable_context_id(std::move(p_callable_context_id)) {}
};

using Scope = std::unordered_map<std::string, Value>;

enum class MemberAccessKind {
	Property,
	Method,
};

bool inferred_annotation(std::string_view value) {
	std::string compact;
	for (char character : value) if (!std::isspace(static_cast<unsigned char>(character))) compact += character;
	return compact == ":=";
}

bool nullable_return_type(const ResolvedType &type) {
	return type.kind == TypeKind::Variant || type.kind == TypeKind::NativeClass || type.kind == TypeKind::ScriptClass;
}

std::optional<std::string> string_literal_value(std::string value) {
	value = trim(value);
	if (value.starts_with('&')) value.erase(value.begin());
	if (value.size() < 2 || (value.front() != '"' && value.front() != '\'') || value.back() != value.front()) {
		return std::nullopt;
	}
	return value.substr(1, value.size() - 2);
}

} // namespace

class SemanticAnalyzerImpl {
public:
	SemanticAnalyzerImpl(const Workspace &p_workspace, const Document &p_document) : workspace(p_workspace), document(p_document), suppressions(p_document) {}

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
	WarningSuppressions suppressions;
	struct FunctionBindings {
		std::vector<std::shared_ptr<Binding>> declarations;
		bool damaged = false;
	};
	std::vector<FunctionBindings> functions;

	void warn(std::string code, std::string message, Range range) {
		auto level = workspace.warning_policy_.level(code, document.resource_path());
		if (level == WarningLevel::Ignore || suppressions.contains(code, range.start)) return;
		add(std::move(code), std::move(message), range,
			level == WarningLevel::Error ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning);
	}

	void declare(const SyntaxNode &name, Value value, std::string unused = {}) {
		auto spelling = text(document, name);
		if (!functions.empty()) {
			if (!unused.empty()) {
				value.binding = std::make_shared<Binding>(Binding{spelling, unused, name.range});
				functions.back().declarations.push_back(value.binding);
			}
			if (!functions.back().damaged && current_class) {
				const Symbol *shadow = nullptr;
				for (const auto &member : current_class->members) if (member.name == spelling) { shadow = &member; break; }
				std::string code;
				if (shadow) code = "shadowed-variable";
				else if (current_class->inheritance_error.empty() &&
					(workspace.find_member(*current_class, spelling) || workspace.native_api_.find_member(workspace.native_base(*current_class), spelling)))
					code = "shadowed-variable-base-class";
				else if (workspace.global_classes_.contains(spelling) || workspace.autoloads_.contains(spelling) ||
					workspace.native_api_.has_class(spelling) || workspace.native_api_.has_global_symbol(spelling) ||
					workspace.native_api_.find_utility_function(spelling) || find_gdscript_builtin_function(spelling))
					code = "shadowed-global-identifier";
				if (!code.empty()) warn(code, "The local declaration \"" + spelling + "\" shadows an existing declaration.", name.range);
			}
		}
		scopes.back()[spelling] = std::move(value);
	}

	void finish_function() {
		if (!functions.back().damaged) for (const auto &binding : functions.back().declarations) {
			if (!binding->read && !binding->name.starts_with('_'))
				warn(binding->warning, "The declaration \"" + binding->name + "\" is never used.", binding->range);
		}
		functions.pop_back();
	}


	void add(std::string code, std::string message, Range range,
			DiagnosticSeverity severity = DiagnosticSeverity::Error) {
		diagnostics.emplace_back(std::move(code), std::move(message), range, severity);
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
			if (auto owner = workspace.symbol_owners_.find(symbol.id); owner != workspace.symbol_owners_.end()) {
				result.callable_context_id = owner->second;
			}
			return result;
		}
		std::vector<std::string> stack;
		result.type = workspace.type_of_symbol(symbol, document, position, stack);
		return result;
	}

	Value resolve_name(std::string_view name, Position position, bool call_target, Range range, bool read = true) {
		for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
			if (auto found = scope->find(std::string(name)); found != scope->end()) {
				if (found->second.binding && read) found->second.binding->read = true;
				if (call_target && !found->second.callable) {
					if (auto *utility = workspace.native_api_.find_utility_function(name)) {
						return {{TypeKind::Callable, "Callable"}, {*utility}, true, true};
					}
					if (auto *builtin = find_gdscript_builtin_function(name)) {
						return {{TypeKind::Callable, "Callable"}, {builtin->signature}, true, true};
					}
				}
				return found->second;
			}
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
			result.callable_context_id = current_class->symbol.id;
			return result;
		}
		if (current_class) {
			if (auto *member = workspace.find_member(*current_class, name)) {
				auto result = symbol_value(*member, position);
				if (call_target && result.callable) {
					if (auto *utility = workspace.native_api_.find_utility_function(name)) result.signatures.push_back(*utility);
					if (auto *builtin = find_gdscript_builtin_function(name)) result.signatures.push_back(builtin->signature);
				}
				return result;
			}
			if (auto *member = workspace.find_lexical_member(*current_class, name)) return symbol_value(*member, position);
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
		if (auto *builtin = find_gdscript_builtin_function(name)) {
			return {{TypeKind::Callable, "Callable"}, {builtin->signature}, true, true};
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
			if (type.kind == TypeKind::Builtin || type.kind == TypeKind::Callable || type.kind == TypeKind::Signal) {
				if (auto *constructors = workspace.native_api_.constructors(type.name)) {
					result.signatures = *constructors;
					for (auto &signature : result.signatures) signature.return_type = type.name;
					result.callable = !result.signatures.empty();
				}
				if (type.kind == TypeKind::Callable && result.signatures.empty()) {
					result.signatures.push_back({"Callable", {}, false});
					result.callable = true;
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

	void add_instance_member_access(std::string_view name, const ResolvedType &receiver,
			Range range, bool method) {
		add("instance-member-access", "Cannot access instance " +
			std::string(method ? "method \"" : "property \"") + std::string(name) +
			"\" on class \"" + receiver.display() + "\"; create an instance first.", range);
	}

	Value member_value(const Value &receiver, std::string_view name, Range range,
			MemberAccessKind access = MemberAccessKind::Property) {
		if (!receiver.resolved || !receiver.type.known()) return {};
		if (receiver.type.kind == TypeKind::Variant || (receiver.type.kind == TypeKind::Builtin && receiver.type.name == "Dictionary"))
			return {{TypeKind::Variant, "Variant"}, {}, true, false};
		if (name == "new" && !receiver.type.instance &&
			(receiver.type.kind == TypeKind::ScriptClass || receiver.type.kind == TypeKind::NativeClass)) {
			Value result{{TypeKind::Callable, "Callable"}, {}, true, true};
			if (receiver.type.kind == TypeKind::ScriptClass) {
				auto *record = workspace.find_class(receiver.type.symbol_id);
				auto *constructor = record ? workspace.find_member(*record, "_init") : nullptr;
				if (constructor) result.signatures.push_back(script_signature(*constructor));
				else result.signatures.push_back({});
				result.callable_context_id = receiver.type.symbol_id;
			} else if (auto *constructors = workspace.native_api_.constructors(receiver.type.name)) {
				result.signatures = *constructors;
				if (result.signatures.empty()) result.signatures.push_back({});
			} else {
				result.signatures.push_back({});
			}
			auto return_type = receiver.type.kind == TypeKind::ScriptClass ? receiver.type.symbol_id : receiver.type.name;
			for (auto &signature : result.signatures) signature.return_type = return_type;
			return result;
		}
		if (receiver.type.kind == TypeKind::ScriptClass) {
			if (auto *record = workspace.find_class(receiver.type.symbol_id)) {
				if (auto *member = workspace.find_member(*record, name)) {
					if (!receiver.type.instance && !is_type_level_member(*member)) {
						auto method = member->kind == SymbolKind::Method || member->kind == SymbolKind::Function ||
							member->kind == SymbolKind::Constructor;
						add_instance_member_access(name, receiver.type, range, method);
						return {};
					}
					return symbol_value(*member, range.start);
				}
				auto native = receiver.type.instance ? workspace.native_base(*record) :
					(workspace.native_api_.has_class("GDScript") ? std::string("GDScript") : std::string("Script"));
				if (!native.empty()) if (auto *member = workspace.native_api_.find_member(native, name)) return native_member_value(*member);
			}
		} else if (receiver.type.kind == TypeKind::NativeClass || receiver.type.kind == TypeKind::Builtin ||
			receiver.type.kind == TypeKind::Callable || receiver.type.kind == TypeKind::Signal) {
			if (auto *member = workspace.native_api_.find_member(receiver.type.name, name)) {
				if (!receiver.type.instance && receiver.type.kind == TypeKind::NativeClass &&
						!is_type_level_member(*member)) {
					add_instance_member_access(name, receiver.type, range, member->signature.has_value());
					return {};
				}
				return native_member_value(*member);
			}
		} else if (receiver.type.kind == TypeKind::Enum) {
			if (receiver.type.symbol_id.starts_with("global:") && workspace.native_api_.global_enum_has_value(
					receiver.type.symbol_id.substr(7), name)) {
				return {receiver.type, {}, true, false};
			}
			if (receiver.type.symbol_id.starts_with("nativeenum:")) {
				auto qualified = receiver.type.symbol_id.substr(11);
				auto separator = qualified.rfind('.');
				if (separator != std::string::npos && workspace.native_api_.enum_has_value(
						qualified.substr(0, separator), qualified.substr(separator + 1), name)) {
					return {receiver.type, {}, true, false};
				}
			}
			for (const auto &[id, record] : workspace.classes_) {
				(void)id;
				for (const auto &member : record->members) if (member.id == receiver.type.symbol_id) {
					for (const auto &value : member.children) if (value.name == name) {
						auto result = symbol_value(value, range.start);
						result.type = receiver.type;
						result.type.declaration_id = value.id;
						return result;
					}
				}
			}
			if (auto *member = workspace.native_api_.find_member("Dictionary", name)) return native_member_value(*member);
		}
		if (receiver.type.instance && (receiver.type.kind == TypeKind::ScriptClass ||
				receiver.type.kind == TypeKind::NativeClass)) {
			auto method = access == MemberAccessKind::Method;
			warn(method ? "unsafe-method-access" : "unsafe-property-access",
				(method ? "Method \"" : "Property \"") + std::string(name) +
				"\" is not present on type \"" + receiver.type.display() + "\" (but may be present on a subtype).", range);
			return {};
		}
		add("unknown-member", "Member \"" + std::string(name) + "\" does not exist on type \"" +
			receiver.type.display() + "\".", range);
		return {};
	}

	Value require_value(Value value, Range range) {
		if (value.resolved && value.type.kind == TypeKind::Void) {
			add("void-value", "Cannot use the result of a call that returns void.", range);
			return {};
		}
		return value;
	}

	bool compatible_value(const ResolvedType &expected, const Value &value) const {
		if (expected.name == "Array" && !expected.arguments.empty() && value.array_literal) {
			for (const auto &element : value.elements) if (!compatible_value(expected.arguments.front(), element)) return false;
			return true;
		}
		return !value.resolved || workspace.is_assignable(expected, value.type) || workspace.is_potential_downcast(expected, value.type);
	}

	bool check_array(const ResolvedType &expected, const Value &value, const SyntaxNode &source) {
		if (expected.name != "Array" || expected.arguments.empty() || !value.array_literal) return false;
		const SyntaxNode *array = &source;
		while (array->kind == "parenthesized_expression" && !array->children.empty()) array = &array->children.front();
		size_t index = 0;
		for (const auto &element : array->children) {
			if (element.kind == "comment") continue;
			if (index >= value.elements.size()) break;
			const auto &actual = value.elements[index++];
			if (check_array(expected.arguments.front(), actual, element)) continue;
			if (!compatible_value(expected.arguments.front(), actual))
				add("type-mismatch", "Array element of type \"" + actual.type.display() + "\" is incompatible with \"" + expected.arguments.front().display() + "\".", element.range);
		}
		return true;
	}

	std::string operation(const SyntaxNode &node) const {
		auto left = field(node, "left"), right = field(node, "right");
		if (!left || !right || right->start_byte < left->end_byte) return {};
		auto source = std::string_view(document.source()).substr(left->end_byte, right->start_byte - left->end_byte);
		std::string result;
		bool comment = false;
		for (char c : source) {
			if (c == '\n') comment = false;
			if (c == '#') comment = true;
			if (!comment && c != '\\' && !std::isspace(static_cast<unsigned char>(c))) result += c;
		}
		if (result == "isnot") return "is not";
		if (result == "notin") return "not in";
		return result;
	}

	Value cast_value(const SyntaxNode &node, std::string target_name = {}) {
		auto left = field(node, "left"), right = field(node, "right");
		if (!left || !right) return {};
		auto source = require_value(evaluate(*left), left->range);
		if (target_name.empty()) target_name = text(document, *right);
		auto target = workspace.type_from_name(target_name, current_class);
		if (!target.known()) {
			add("unknown-type", "Could not find cast type \"" + text(document, *right) + "\".", right->range);
			return {};
		}
		if (!source.resolved || !source.type.known()) return {};
		if (source.type.kind != TypeKind::Variant && !workspace.is_assignable(target, source.type) && !workspace.is_potential_downcast(target, source.type)) {
			add("invalid-cast", "Cannot cast \"" + source.type.display() + "\" to \"" + target.display() + "\".", node.range);
			return {};
		}
		target.instance = true;
		return {target, {}, true, false};
	}

	std::vector<const SyntaxNode *> argument_nodes(const SyntaxNode *arguments) const {
		std::vector<const SyntaxNode *> result;
		if (!arguments) return result;
		for (const auto &child : arguments->children) if (child.kind != "comment") result.push_back(&child);
		return result;
	}

	Value call_value(Value callee, const SyntaxNode *arguments, Range range) {
		auto nodes = argument_nodes(arguments);
		std::vector<Value> values;
		for (auto *node : nodes) values.push_back(require_value(evaluate(*node), node->range));
		if (!callee.resolved || std::any_of(values.begin(), values.end(), [](const Value &value) { return !value.resolved; })) return {};
		if (!callee.callable && callee.type.kind == TypeKind::Variant) return {{TypeKind::Variant, "Variant"}, {}, true, false};
		if (!callee.callable) {
			if (callee.type.kind != TypeKind::Variant && callee.type.known()) {
				add("not-callable", "A value of type \"" + callee.type.display() + "\" is not callable.", range);
			}
			return {};
		}
		auto *signature_context = callee.callable_context_id.empty() ? current_class :
			workspace.find_class(callee.callable_context_id);
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
				auto expected = workspace.type_from_name(signature->arguments[index].type, signature_context);
				if (expected.known() && values[index].type.known() &&
					(!workspace.is_assignable(expected, values[index].type) || (values[index].array_literal && !compatible_value(expected, values[index])))) {
					compatible = false;
					break;
				}
			}
			if (compatible) {
				auto result_type = workspace.type_from_name(signature->return_type, signature_context);
				// An unavailable return type stays unknown rather than becoming dynamic.
				return {result_type, {}, true, false};
			}
		}
		for (auto *signature : arity_matches) {
			bool compatible = true;
			for (size_t index = 0; index < values.size() && index < signature->arguments.size(); ++index) {
				auto expected = workspace.type_from_name(signature->arguments[index].type, signature_context);
				if ((values[index].array_literal && !compatible_value(expected, values[index])) ||
					(expected.known() && values[index].type.known() &&
						!workspace.is_assignable(expected, values[index].type) &&
						!workspace.is_potential_downcast(expected, values[index].type))) {
					compatible = false;
					break;
				}
			}
			if (!compatible) continue;
			for (size_t index = 0; index < values.size() && index < signature->arguments.size(); ++index) {
				auto expected = workspace.type_from_name(signature->arguments[index].type, signature_context);
				if (!workspace.is_potential_downcast(expected, values[index].type) ||
						workspace.warning_policy_.level("unsafe-call-argument", document.resource_path()) == WarningLevel::Ignore) continue;
				warn("unsafe-call-argument", "Argument " + std::to_string(index + 1) + " expects \"" + expected.display() +
					"\", but the value has the broader type \"" + values[index].type.display() + "\".", nodes[index]->range);
			}
			auto result_type = workspace.type_from_name(signature->return_type, signature_context);
			// An unavailable return type stays unknown rather than becoming dynamic.
			return {result_type, {}, true, false};
		}
		auto *signature = arity_matches.front();
		for (size_t index = 0; index < values.size() && index < signature->arguments.size(); ++index) {
			auto expected = workspace.type_from_name(signature->arguments[index].type, signature_context);
			if (check_array(expected, values[index], *nodes[index])) continue;
			if (expected.known() && values[index].type.known() && !workspace.is_assignable(expected, values[index].type)) {
				add("argument-type", "Argument " + std::to_string(index + 1) + " expects \"" + expected.display() +
					"\", but received \"" + values[index].type.display() + "\".", nodes[index]->range);
				break;
			}
		}
		auto result_type = workspace.type_from_name(signature->return_type, signature_context);
		return {result_type, {}, true, false};
	}

	Value unary_result(const SyntaxNode &node, Value value) {
		const auto &operand = node.children.back();
		if (!value.resolved || !value.type.known()) return {};
		auto op = trim(std::string_view(document.source()).substr(node.start_byte, operand.start_byte - node.start_byte));
		if (op == "!" || op == "not") return {{TypeKind::Builtin, "bool"}, {}, true, false};
		if (value.type.kind == TypeKind::Variant) return value;
		if (op == "+" || op == "-") op = "unary" + op;
		auto name = value.type.kind == TypeKind::Enum ? "int" : value.type.name;
		if (auto native = workspace.native_api_.find_class(name); native && native->operators_known) {
			for (const auto &entry : native->operators) if (entry.name == op && entry.right_type.empty())
				return {workspace.type_from_name(entry.return_type, current_class), {}, true, false};
			add("invalid-operator", "Invalid operand for unary operator \"" + op + "\".", node.range);
			return {};
		}
		return value;
	}

	Value binary_result(const SyntaxNode &node, Value left_value, Value right_value) {
		auto op = operation(node);
		if (node.kind == "augmented_assignment" && op.ends_with('=')) op.pop_back();
		if (!left_value.resolved || !right_value.resolved || !left_value.type.known() || !right_value.type.known()) return {};
		if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" ||
			op == "is" || op == "is not" || op == "in" || op == "not in" || op == "and" || op == "or" || op == "&&" || op == "||")
			return {{TypeKind::Builtin, "bool"}, {}, true, false};
		if (left_value.type.kind == TypeKind::Variant || right_value.type.kind == TypeKind::Variant)
			return {{TypeKind::Variant, "Variant"}, {}, true, false};
		auto left_name = left_value.type.kind == TypeKind::Enum ? "int" : left_value.type.name;
		auto right_name = right_value.type.kind == TypeKind::Enum ? "int" : right_value.type.name;
		if (right_value.type.kind == TypeKind::NativeClass || right_value.type.kind == TypeKind::ScriptClass) right_name = "Object";
		auto *native = workspace.native_api_.find_class(left_name);
		if (native && native->operators_known) {
			const NativeOperator *wildcard = nullptr;
			for (const auto &entry : native->operators) if (entry.name == op) {
				if (entry.right_type == right_name) return {workspace.type_from_name(entry.return_type, current_class), {}, true, false};
				if (entry.right_type == "Variant") wildcard = &entry;
			}
			if (wildcard) return {workspace.type_from_name(wildcard->return_type, current_class), {}, true, false};
			add("invalid-operator", "Invalid operands \"" + left_value.type.display() + "\" and \"" + right_value.type.display() + "\" for operator \"" + op + "\".", node.range);
			return {};
		}
		// Old reduced snapshots do not contain operator tables. Keep completion-era
		// inference, but do not manufacture incompatibility diagnostics.
		if (left_value.type.name == right_value.type.name) return left_value;
		return {};
	}

	Value apply_attribute_nodes(Value current, const std::vector<const SyntaxNode *> &parts) {
		for (const auto *part_ptr : parts) {
			const auto &part = *part_ptr;
			if (part.kind == "identifier") {
				current = member_value(current, text(document, part), part.range);
			} else if (part.kind == "attribute_call") {
				auto *identifier = first_identifier(part);
				if (!identifier) return {};
				auto receiver = current;
				auto arguments = field(part, "arguments");
				current = call_value(member_value(receiver, text(document, *identifier), identifier->range,
					MemberAccessKind::Method), arguments, part.range);
				if (receiver.type.kind == TypeKind::NativeClass && receiver.type.name == "Engine" &&
						text(document, *identifier) == "get_singleton" && arguments &&
						arguments->children.size() == 1) {
					if (auto singleton_name = string_literal_value(text(document, arguments->children.front()))) {
						if (auto singleton_type = workspace.native_api_.singleton_type(*singleton_name)) {
							current = {{TypeKind::NativeClass, *singleton_type, "native:" + *singleton_type, true}, {}, true, false};
						}
					}
				}
			} else if (part.kind == "attribute_subscript") {
				auto *identifier = first_identifier(part);
				current = identifier ? member_value(current, text(document, *identifier), identifier->range) : Value{};
				if (auto *arguments = field(part, "arguments")) {
					for (const auto &child : arguments->children) require_value(evaluate(child), child.range);
				}
				if (current.type.name == "Array" && !current.type.arguments.empty()) {
					auto element = current.type.arguments.front();
					current = {element, {}, true, false};
				} else current = {{TypeKind::Variant, "Variant"}, {}, true, false};
			}
		}
		return current;
	}

	Value apply_attribute_parts(Value current, const SyntaxNode &node, size_t start) {
		std::vector<const SyntaxNode *> parts;
		for (size_t index = start; index < node.children.size(); ++index) parts.push_back(&node.children[index]);
		return apply_attribute_nodes(std::move(current), parts);
	}

	Value evaluate_with_attribute_suffix(const SyntaxNode &node, std::vector<const SyntaxNode *> suffix) {
		if (node.kind == "attribute" && !node.children.empty() && (node.children.front().kind == "binary_operator" || node.children.front().kind == "unary_operator")) {
			std::vector<const SyntaxNode *> combined;
			for (size_t index = 1; index < node.children.size(); ++index) combined.push_back(&node.children[index]);
			combined.insert(combined.end(), suffix.begin(), suffix.end());
			return evaluate_with_attribute_suffix(node.children.front(), std::move(combined));
		}
		if (node.kind == "unary_operator" && !node.children.empty()) {
			const auto &operand = node.children.back();
			return unary_result(node, require_value(evaluate_with_attribute_suffix(operand, std::move(suffix)), operand.range));
		}
		if (node.kind == "binary_operator") {
			if (operation(node) == "as") {
				auto right = field(node, "right");
				if (right && !suffix.empty()) return cast_value(node, trim(std::string_view(document.source()).substr(right->start_byte, suffix.back()->end_byte - right->start_byte)));
				return cast_value(node);
			}
			auto *left = field(node, "left");
			auto *right = field(node, "right");
			auto left_value = left ? require_value(evaluate(*left), left->range) : Value{};
			auto right_value = right ? evaluate_with_attribute_suffix(*right, std::move(suffix)) : Value{};
			return binary_result(node, std::move(left_value), std::move(right_value));
		}
		return apply_attribute_nodes(evaluate(node), suffix);
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
			Value result{{TypeKind::Builtin, "Array"}, {}, true, false};
			result.array_literal = true;
			for (const auto &child : node.children) if (child.kind != "comment") result.elements.push_back(require_value(evaluate(child), child.range));
			return result;
		}
		if (node.kind == "dictionary") {
			for (const auto &child : node.children) evaluate(child);
			return {{TypeKind::Builtin, "Dictionary"}, {}, true, false};
		}
		if (node.kind == "pair") {
			for (const auto &child : node.children) require_value(evaluate(child), child.range);
			return {{TypeKind::Variant, "Variant"}, {}, true, false};
		}
		if (node.kind == "parenthesized_expression") return node.children.empty() ? Value{} : evaluate(node.children.front());
		if (node.kind == "await_expression") return node.children.empty() ? Value{} : require_value(evaluate(node.children.front()), node.children.front().range);
		if (node.kind == "unary_operator") {
			if (node.children.empty()) return {};
			return unary_result(node, require_value(evaluate(node.children.back()), node.children.back().range));
		}
		if (node.kind == "binary_operator") {
			if (operation(node) == "as") return cast_value(node);
			auto *left = field(node, "left");
			auto *right = field(node, "right");
			auto left_value = left ? require_value(evaluate(*left), left->range) : Value{};
			auto right_value = right ? ((operation(node) == "is" || operation(node) == "is not") ?
				Value{workspace.type_from_name(text(document, *right), current_class), {}, true, false} : require_value(evaluate(*right), right->range)) : Value{};
			return binary_result(node, std::move(left_value), std::move(right_value));
		}
		if (node.kind == "conditional_expression") {
			if (auto *condition = field(node, "condition")) require_value(evaluate(*condition), condition->range);
			auto left = field(node, "left") ? require_value(evaluate(*field(node, "left")), field(node, "left")->range) : Value{};
			auto right = field(node, "right") ? require_value(evaluate(*field(node, "right")), field(node, "right")->range) : Value{};
			auto aligned = left.type.known() && right.type.known() && left.type.kind == right.type.kind &&
				left.type.name == right.type.name && left.type.instance == right.type.instance &&
				(left.type.symbol_id.empty() || right.type.symbol_id.empty() || left.type.symbol_id == right.type.symbol_id);
			return aligned ? left :
				Value{{TypeKind::Variant, "Variant"}, {}, true, false};
		}
		if (node.kind == "assignment" || node.kind == "augmented_assignment") {
			auto left = field(node, "left"), right = field(node, "right");
			Value target;
			if (left) {
				if (left->kind == "identifier") target = resolve_name(text(document, *left), left->range.start, false, left->range, node.kind == "augmented_assignment");
				else target = evaluate(*left);
				if (target.binding) target.binding->written = true;
			}
			auto value = right ? require_value(evaluate(*right), right->range) : Value{};
			if (node.kind == "augmented_assignment") value = binary_result(node, target, std::move(value));
			bool indexed = left && (left->kind == "subscript" || (left->kind == "attribute" && !left->children.empty() && left->children.back().kind == "attribute_subscript"));
			if (right && !check_array(target.type, value, *right) && indexed &&
				!compatible_value(target.type, value)) add("type-mismatch", "Assigned value is incompatible with the array element type.", right->range);
			return value;
		}
		if (node.kind == "subscript") {
			auto base = node.children.empty() ? Value{} : evaluate(node.children.front());
			if (auto *arguments = field(node, "arguments")) for (const auto &child : arguments->children) require_value(evaluate(child), child.range);
			if (base.type.kind == TypeKind::Builtin && base.type.name == "Array" && !base.type.arguments.empty()) {
				return {base.type.arguments.front(), {}, true, false};
			}
			if (base.type.kind == TypeKind::Builtin && base.type.name == "String") return {{TypeKind::Builtin, "String"}, {}, true, false};
			return {{TypeKind::Variant, "Variant"}, {}, true, false};
		}
		if (node.kind == "call") {
			const SyntaxNode *callee_node = nullptr;
			for (const auto &child : node.children) if (child.field != "arguments") { callee_node = &child; break; }
			if (callee_node && callee_node->kind == "identifier" && text(document, *callee_node) == "yield") return {};
			auto callee = callee_node ? evaluate(*callee_node, callee_node->kind == "identifier") : Value{};
			auto result = call_value(std::move(callee), field(node, "arguments"), node.range);
			if (callee_node && callee_node->kind == "identifier" &&
					(text(document, *callee_node) == "load" || text(document, *callee_node) == "preload")) {
				auto nodes = argument_nodes(field(node, "arguments"));
				if (nodes.size() == 1) if (auto path = string_literal_value(text(document, *nodes.front()))) {
					return {workspace.type_for_resource_path(*path, current_class), {}, true, false};
				}
			}
			return result;
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
				callee = member_value(base, text(document, *identifier), identifier->range, MemberAccessKind::Method);
			}
			return call_value(std::move(callee), field(node, "arguments"), node.range);
		}
		if (node.kind == "attribute") {
			if (node.children.empty()) return {};
			if ((node.children.front().kind == "binary_operator" || node.children.front().kind == "unary_operator") && node.children.size() > 1) {
				std::vector<const SyntaxNode *> suffix;
				for (size_t index = 1; index < node.children.size(); ++index) suffix.push_back(&node.children[index]);
				return evaluate_with_attribute_suffix(node.children.front(), std::move(suffix));
			}
			return apply_attribute_parts(evaluate(node.children.front()), node, 1);
		}
		if (node.kind == "lambda") {
			analyze_lambda(node);
			Value result{{TypeKind::Callable, "Callable"}, {syntax_signature(node)}, true, true};
			if (current_class) result.callable_context_id = current_class->symbol.id;
			return result;
		}
		if (node.kind == "annotation" || node.kind == "annotations" || node.kind == "comment" || node.kind == "type") return {};
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
			if (auto *value = field(parameter, "value")) require_value(evaluate(*value), value->range);
			ResolvedType type{TypeKind::Variant, "Variant"};
			if (function) for (const auto &child : function->children) {
				if (child.is_parameter && child.name == text(document, *identifier)) {
					if (child.is_inferred) {
						std::vector<std::string> stack;
						type = workspace.type_of_symbol(child, document, child.range.start, stack);
					} else {
						type = workspace.type_from_name(child.declared_type, current_class);
					}
					if (!type.known()) type = {TypeKind::Variant, "Variant"};
					break;
				}
			} else if (auto *type_node = field(parameter, "type"); type_node && !inferred_annotation(text(document, *type_node))) {
				type = workspace.type_from_name(text(document, *type_node), current_class);
				if (!type.known()) {
					auto declared = text(document, *type_node);
					if (auto message = workspace.invalid_type_message(declared, current_class)) {
						add("invalid-type", *message, type_node->range);
					} else {
						add("unknown-type", "Could not find type \"" + declared + "\" in the current scope.",
							type_node->range);
					}
					type = {TypeKind::Variant, "Variant"};
				}
			}
			declare(*identifier, {type, {}, true, false}, "unused-parameter");
		}
	}

	void analyze_function(const SyntaxNode &node) {
		auto *saved_class = current_class;
		current_class = document.class_at(node.range.start);
		auto *function = function_symbol(node);
		auto *body = field(node, "body");
		if (!body) { current_class = saved_class; return; }
		scopes.emplace_back();
		if (node.has_error || body->has_error) for (auto &frame : functions) frame.damaged = true;
		functions.push_back({{}, node.has_error || body->has_error});
		bind_parameters(field(node, "parameters"), function);
		auto expected = function && !function->declared_type.empty() ? workspace.type_from_name(function->declared_type, current_class) : ResolvedType{};
		analyze_block(*body, expected);
		if (function && !body->has_error && expected.known() && expected.kind != TypeKind::Void && !always_returns(*body)) {
			add("missing-return-path", "Not all code paths return a value of type \"" + expected.display() + "\".", function->selection_range);
		}
		finish_function();
		scopes.pop_back();
		current_class = saved_class;
	}

	void analyze_lambda(const SyntaxNode &node) {
		auto *body = field(node, "body");
		if (!body) return;
		scopes.emplace_back();
		if (node.has_error || body->has_error) for (auto &frame : functions) frame.damaged = true;
		functions.push_back({{}, node.has_error || body->has_error});
		bind_parameters(field(node, "parameters"), nullptr);
		ResolvedType expected;
		if (auto *type = field(node, "return_type")) expected = workspace.type_from_name(text(document, *type), current_class);
		analyze_block(*body, expected);
		if (!body->has_error && expected.known() && expected.kind != TypeKind::Void && !always_returns(*body)) {
			add("missing-return-path", "Not all lambda code paths return a value of type \"" + expected.display() + "\".", node.range);
		}
		finish_function();
		scopes.pop_back();
	}

	void analyze_accessor(const SyntaxNode &node, const ResolvedType &expected) {
		auto *body = field(node, "body");
		if (!body) return;
		scopes.emplace_back();
		if (node.has_error || body->has_error) for (auto &frame : functions) frame.damaged = true;
		functions.push_back({{}, node.has_error || body->has_error});
		bind_parameters(child_kind(node, "parameters"), nullptr);
		analyze_block(*body, expected);
		if (!body->has_error && expected.known() && expected.kind != TypeKind::Void && !always_returns(*body)) {
			add("missing-return-path", "Not all accessor code paths return a value of type \"" + expected.display() + "\".", node.range);
		}
		finish_function();
		scopes.pop_back();
	}

	void bind_local(const SyntaxNode &node, Value initializer) {
		if (scopes.empty()) return;
		auto *name_node = field(node, "name");
		if (!name_node) return;
		ResolvedType type{TypeKind::Variant, "Variant"};
		if (auto *type_node = field(node, "type")) {
			auto declared = text(document, *type_node);
			if (inferred_annotation(declared)) {
				type = initializer.type;
			} else {
				type = workspace.type_from_name(declared, current_class);
			}
		} else if (node.kind == "const_statement" && initializer.type.known()) {
			type = initializer.type;
		}
		declare(*name_node, {type, {}, type.known(), false}, node.kind == "const_statement" ? "unused-local-constant" : "unused-variable");
	}

	void check_declared_assignment(const SyntaxNode &node, const Value &initializer) {
		auto *type_node = field(node, "type");
		if (!type_node) return;
		auto declared = text(document, *type_node);
		if (declared.empty() || inferred_annotation(declared)) return;
		auto expected = workspace.type_from_name(declared, current_class);
		if (auto value = field(node, "value"); value && check_array(expected, initializer, *value)) return;
		if (expected.kind == TypeKind::Void) return;
		if (!expected.known() || !initializer.type.known() || initializer.type.kind == TypeKind::Variant) return;
		if (workspace.is_potential_downcast(expected, initializer.type)) {
			return;
		} else if (!workspace.is_assignable(expected, initializer.type)) {
			add("type-mismatch", "Cannot assign a value of type \"" + initializer.type.display() +
				"\" to \"" + declared + "\".", node.range);
		}
	}

	void annotation_reads(const SyntaxNode &node) {
		if (node.kind == "identifier") {
			for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
				auto found = scope->find(text(document, node));
				if (found != scope->end()) { if (found->second.binding) found->second.binding->read = true; break; }
			}
		}
		for (const auto &child : node.children) if (node.kind != "annotation" || child.field == "arguments") annotation_reads(child);
	}

	void analyze_block(const SyntaxNode &block, const ResolvedType &expected_return) {
		unsigned flow = FallsThrough;
		bool reported = false;
		for (const auto &statement : block.children) {
			if (statement.kind == "comment") continue;
			if (statement.kind == "annotation") { annotation_reads(statement); continue; }
			if (!(flow & FallsThrough) && !reported && !block.has_error && (functions.empty() || !functions.back().damaged)) {
				warn("unreachable-code", "This statement cannot be reached.", statement.range);
				reported = true;
			}
			analyze_statement(statement, expected_return);
			if (flow & FallsThrough) flow = (flow & ~FallsThrough) | statement_flow(statement, document);
		}
	}

	void analyze_scoped_body(const SyntaxNode *body, const ResolvedType &expected_return,
		std::optional<std::pair<std::string, Value>> binding = std::nullopt) {
		if (!body) return;
		scopes.emplace_back();
		if (binding) scopes.back()[binding->first] = binding->second;
		analyze_block(*body, expected_return);
		scopes.pop_back();
	}

	bool is_call(const SyntaxNode &node) const {
		if (node.kind == "call" || node.kind == "base_call") return true;
		if ((node.kind == "parenthesized_expression" || node.kind == "await_expression") && !node.children.empty()) return is_call(node.children.front());
		return node.kind == "attribute" && !node.children.empty() && node.children.back().kind == "attribute_call";
	}

	void analyze_statement(const SyntaxNode &node, const ResolvedType &expected_return) {
		for (const auto &child : node.children) if (child.kind == "annotations") annotation_reads(child);
		if (node.kind == "variable_statement" || node.kind == "const_statement" ||
			node.kind == "export_variable_statement" || node.kind == "onready_variable_statement") {
			if (node.has_error) return;
			Value initializer;
			if (auto *value = field(node, "value")) initializer = require_value(evaluate(*value), value->range);
			check_declared_assignment(node, initializer);
			bind_local(node, initializer);
			return;
		}
		if (node.kind == "expression_statement") {
			if (node.has_error) return;
			for (const auto &child : node.children) evaluate(child);
			return;
		}
		if (node.kind == "return_statement") {
			if (node.has_error) return;
			const SyntaxNode *value_node = node.children.empty() ? nullptr : &node.children.front();
			if (!expected_return.known()) { if (value_node) evaluate(*value_node); return; }
			if (expected_return.kind == TypeKind::Void && value_node) {
				auto value = evaluate(*value_node);
				if (value.resolved && value.type.kind == TypeKind::Variant && is_call(*value_node))
					warn("unsafe-void-return", "The returned call is not guaranteed to return void.", node.range);
				else if (value.resolved && value.type.known() && value.type.kind != TypeKind::Void)
					add("return-value-in-void", "A void function cannot return a value.", node.range);
			} else if (expected_return.kind != TypeKind::Void && !value_node && !nullable_return_type(expected_return)) {
				add("missing-return-value", "A value of type \"" + expected_return.display() + "\" must be returned.", node.range);
			} else if (value_node) {
				auto value = require_value(evaluate(*value_node), value_node->range);
				if (check_array(expected_return, value, *value_node)) return;
				auto actual = value.type;
				if (actual.known() && actual.kind != TypeKind::Variant && workspace.is_potential_downcast(expected_return, actual)) {
					return;
				} else if (actual.known() && actual.kind != TypeKind::Variant && !workspace.is_assignable(expected_return, actual)) {
					add("return-type-mismatch", "Cannot return \"" + actual.display() + "\" from a function returning \"" +
						expected_return.display() + "\".", value_node->range);
				}
			}
			return;
		}
		if (node.kind == "if_statement") {
			if (auto *condition = field(node, "condition")) require_value(evaluate(*condition), condition->range);
			analyze_scoped_body(field(node, "body"), expected_return);
			for (const auto &alternative : node.children) if (alternative.field == "alternative") {
				if (auto *condition = field(alternative, "condition")) require_value(evaluate(*condition), condition->range);
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
			if (auto *condition = field(node, "condition")) require_value(evaluate(*condition), condition->range);
			analyze_scoped_body(field(node, "body"), expected_return);
			return;
		}
		if (node.kind == "match_statement") {
			if (auto *value = field(node, "value")) evaluate(*value);
			auto *match_body = field(node, "body");
			bool covered = false;
			if (match_body) for (const auto &section : match_body->children) if (section.kind == "pattern_section") {
				if (covered && !match_body->has_error && (functions.empty() || !functions.back().damaged))
					warn("unreachable-pattern", "This pattern cannot be reached after a catch-all pattern.",
						{section.range.start, field(section, "body") ? field(section, "body")->range.start : section.range.end});
				covered |= catch_all_pattern(section, document);
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
		return statement_flow(node, document) == Returns;
	}

	void analyze_class_container(const SyntaxNode &container) {
		for (const auto &node : container.children) {
			if (node.kind == "function_definition" || node.kind == "constructor_definition") analyze_function(node);
			else if (node.kind == "class_definition") {
				if (auto *body = field(node, "body")) analyze_class_container(*body);
			} else if (node.kind == "variable_statement" || node.kind == "const_statement" ||
				node.kind == "export_variable_statement" || node.kind == "onready_variable_statement") {
				if (node.has_error) continue;
				auto *saved = current_class;
				current_class = document.class_at(node.range.start);
				Value initializer;
				if (auto *value = field(node, "value")) initializer = require_value(evaluate(*value), value->range);
				check_declared_assignment(node, initializer);
				ResolvedType property_type;
				if (auto *type_node = field(node, "type"); type_node && !inferred_annotation(text(document, *type_node))) {
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
