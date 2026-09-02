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

const std::vector<std::string_view> &gdscript_reserved_words() {
	// Godot 4.6: GDScriptLanguage::get_reserved_words(). The standalone "_"
	// token is also rejected wherever the parser requires an IDENTIFIER.
	static const std::vector<std::string_view> words = {
		"break", "continue", "elif", "else", "for", "if", "match", "pass", "return", "when", "while",
		"class", "class_name", "const", "enum", "extends", "func", "namespace", "signal", "static", "trait", "var",
		"await", "breakpoint", "self", "super", "yield",
		"and", "as", "in", "is", "not", "or",
		"false", "null", "true",
		"INF", "NAN", "PI", "TAU",
		"assert", "preload", "void", "_",
	};
	return words;
}

bool is_gdscript_reserved_identifier(std::string_view name) {
	const auto &words = gdscript_reserved_words();
	return std::find(words.begin(), words.end(), name) != words.end();
}

} // namespace gdscript_lsp
