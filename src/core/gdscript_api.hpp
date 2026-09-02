#pragma once

#include "core/native_api.hpp"

#include <string_view>
#include <vector>

namespace gdscript_lsp {

struct GDScriptBuiltinFunction {
	std::string_view name;
	std::string_view detail;
	CallableSignature signature;
};

const GDScriptBuiltinFunction *find_gdscript_builtin_function(std::string_view name);
const std::vector<GDScriptBuiltinFunction> &gdscript_builtin_functions();
const std::vector<std::string_view> &gdscript_reserved_words();
bool is_gdscript_reserved_identifier(std::string_view name);

} // namespace gdscript_lsp
