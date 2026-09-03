#include "core/types.hpp"

namespace gdscript_lsp {

ResolvedType::ResolvedType(TypeKind p_kind, std::string p_name, std::string p_symbol_id,
		bool p_instance, std::vector<ResolvedType> p_arguments) :
		kind(p_kind), name(std::move(p_name)), symbol_id(std::move(p_symbol_id)), instance(p_instance),
		arguments(std::move(p_arguments)) {}

Diagnostic::Diagnostic(std::string p_code, std::string p_message, Range p_range,
		DiagnosticSeverity p_severity) :
		code(std::move(p_code)), message(std::move(p_message)), range(p_range), severity(p_severity) {}

std::string ResolvedType::display() const {
	if (name.empty()) {
		return kind == TypeKind::Unknown ? "Unknown" : std::string(type_kind_name(kind));
	}
	if (arguments.empty()) {
		return name;
	}
	std::string result = name + "[";
	for (size_t index = 0; index < arguments.size(); ++index) {
		if (index != 0) result += ", ";
		result += arguments[index].display();
	}
	return result + "]";
}

ResolvedType ResolvedType::unknown(std::string reason) {
	ResolvedType value;
	value.name = std::move(reason);
	return value;
}

std::string_view type_kind_name(TypeKind kind) {
	switch (kind) {
		case TypeKind::Unknown: return "unknown";
		case TypeKind::Variant: return "variant";
		case TypeKind::Void: return "void";
		case TypeKind::Builtin: return "builtin";
		case TypeKind::NativeClass: return "native_class";
		case TypeKind::ScriptClass: return "script_class";
		case TypeKind::Enum: return "enum";
		case TypeKind::Callable: return "callable";
		case TypeKind::Signal: return "signal";
	}
	return "unknown";
}

std::string_view access_path_kind_name(AccessPathKind kind) {
	switch (kind) {
		case AccessPathKind::ScriptAlias: return "scriptAlias";
		case AccessPathKind::Local: return "local";
		case AccessPathKind::Global: return "global";
		case AccessPathKind::Native: return "native";
	}
	return "local";
}

} // namespace gdscript_lsp
