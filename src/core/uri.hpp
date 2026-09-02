#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gdscript_lsp {

std::string file_uri_for_path(const std::filesystem::path &path);
std::optional<std::filesystem::path> path_for_file_uri(std::string_view uri);

} // namespace gdscript_lsp
