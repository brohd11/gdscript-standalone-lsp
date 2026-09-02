#include "core/uri.hpp"

#include <cctype>

namespace gdscript_lsp {
namespace {

int hex_value(char value) {
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

std::optional<std::string> percent_decode(std::string_view input) {
	std::string result;
	result.reserve(input.size());
	for (size_t index = 0; index < input.size(); ++index) {
		if (input[index] != '%') {
			if (input[index] == '\0') return std::nullopt;
			result.push_back(input[index]);
			continue;
		}
		if (index + 2 >= input.size()) return std::nullopt;
		auto high = hex_value(input[index + 1]);
		auto low = hex_value(input[index + 2]);
		if (high < 0 || low < 0) return std::nullopt;
		auto decoded = static_cast<char>((high << 4) | low);
		if (decoded == '\0') return std::nullopt;
		result.push_back(decoded);
		index += 2;
	}
	return result;
}

std::string percent_encode(std::string_view input) {
	constexpr char hex[] = "0123456789ABCDEF";
	std::string result;
	for (unsigned char value : input) {
		if (std::isalnum(value) || value == '/' || value == '-' || value == '_' || value == '.' || value == '~' ||
			value == ':') {
			result.push_back(static_cast<char>(value));
		} else {
			result += '%';
			result += hex[value >> 4U];
			result += hex[value & 15U];
		}
	}
	return result;
}

} // namespace

std::string file_uri_for_path(const std::filesystem::path &path) {
	auto value = std::filesystem::absolute(path).lexically_normal().generic_string();
#ifdef _WIN32
	if (value.size() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':') value.insert(0, "/");
#endif
	return "file://" + percent_encode(value);
}

std::optional<std::filesystem::path> path_for_file_uri(std::string_view uri) {
	if (!uri.starts_with("file://")) return std::nullopt;
	auto value = uri.substr(7);
	if (!value.starts_with('/')) {
		auto slash = value.find('/');
		if (slash == std::string_view::npos) return std::nullopt;
		auto authority = value.substr(0, slash);
		if (authority != "localhost") return std::nullopt;
		value.remove_prefix(slash);
	}
	auto decoded = percent_decode(value);
	if (!decoded) return std::nullopt;
#ifdef _WIN32
	if (decoded->size() >= 3 && (*decoded)[0] == '/' && std::isalpha(static_cast<unsigned char>((*decoded)[1])) &&
		(*decoded)[2] == ':') {
		decoded->erase(0, 1);
	}
#endif
	return std::filesystem::path(*decoded).lexically_normal();
}

} // namespace gdscript_lsp
