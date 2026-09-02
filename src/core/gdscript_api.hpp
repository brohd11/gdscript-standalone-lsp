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

} // namespace gdscript_lsp
