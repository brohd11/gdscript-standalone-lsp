#include "core/gdscript_api.hpp"

#include <algorithm>

namespace gdscript_lsp {

const std::vector<GDScriptBuiltinFunction> &gdscript_builtin_functions() {
	static const std::vector<GDScriptBuiltinFunction> functions = {
		{"char", "func char(char: int) -> String", {"String", {{"char", "int", false}}, false}},
		{"is_instance_of", "func is_instance_of(value: Variant, type: Variant) -> bool",
			{"bool", {{"value", "Variant", false}, {"type", "Variant", false}}, false}},
		{"len", "func len(var: Variant) -> int", {"int", {{"var", "Variant", false}}, false}},
	};
	return functions;
}

const GDScriptBuiltinFunction *find_gdscript_builtin_function(std::string_view name) {
	const auto &functions = gdscript_builtin_functions();
	auto found = std::find_if(functions.begin(), functions.end(),
		[&](const GDScriptBuiltinFunction &function) { return function.name == name; });
	return found == functions.end() ? nullptr : &*found;
}

} // namespace gdscript_lsp
