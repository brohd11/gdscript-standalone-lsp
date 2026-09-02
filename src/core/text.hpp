#pragma once

#include "core/types.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace gdscript_lsp {

size_t position_to_byte(std::string_view text, Position position);
Position byte_to_position(std::string_view text, size_t byte_offset);
std::string identifier_at(std::string_view text, Position position);
std::string trim(std::string_view text);
std::string unquote(std::string_view text);

} // namespace gdscript_lsp
