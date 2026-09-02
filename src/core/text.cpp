#include "core/text.hpp"

#include <algorithm>
#include <cctype>

namespace gdscript_lsp {
namespace {

size_t utf8_width(unsigned char c) {
	if ((c & 0x80U) == 0) return 1;
	if ((c & 0xE0U) == 0xC0U) return 2;
	if ((c & 0xF0U) == 0xE0U) return 3;
	if ((c & 0xF8U) == 0xF0U) return 4;
	return 1;
}

uint32_t codepoint(std::string_view text, size_t offset, size_t width) {
	const auto c = static_cast<unsigned char>(text[offset]);
	if (width == 1) return c;
	uint32_t value = c & ((1U << (7U - static_cast<unsigned>(width))) - 1U);
	for (size_t i = 1; i < width && offset + i < text.size(); ++i) {
		value = (value << 6U) | (static_cast<unsigned char>(text[offset + i]) & 0x3FU);
	}
	return value;
}

bool identifier_char(char value) {
	return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

} // namespace

size_t position_to_byte(std::string_view text, Position position) {
	size_t offset = 0;
	for (uint32_t line = 0; line < position.line && offset < text.size(); ++line) {
		auto newline = text.find('\n', offset);
		offset = newline == std::string_view::npos ? text.size() : newline + 1;
	}
	uint32_t utf16 = 0;
	while (offset < text.size() && text[offset] != '\n' && utf16 < position.character) {
		size_t width = std::min(utf8_width(static_cast<unsigned char>(text[offset])), text.size() - offset);
		utf16 += codepoint(text, offset, width) > 0xFFFFU ? 2U : 1U;
		offset += width;
	}
	return offset;
}

Position byte_to_position(std::string_view text, size_t byte_offset) {
	byte_offset = std::min(byte_offset, text.size());
	Position result;
	size_t offset = 0;
	while (offset < byte_offset) {
		if (text[offset] == '\n') {
			++result.line;
			result.character = 0;
			++offset;
			continue;
		}
		size_t width = std::min(utf8_width(static_cast<unsigned char>(text[offset])), byte_offset - offset);
		result.character += codepoint(text, offset, width) > 0xFFFFU ? 2U : 1U;
		offset += width;
	}
	return result;
}

std::string identifier_at(std::string_view text, Position position) {
	size_t offset = position_to_byte(text, position);
	if (offset == text.size() || !identifier_char(text[offset])) {
		if (offset == 0 || !identifier_char(text[offset - 1])) return {};
		--offset;
	}
	size_t start = offset;
	while (start > 0 && identifier_char(text[start - 1])) --start;
	size_t end = offset;
	while (end < text.size() && identifier_char(text[end])) ++end;
	return std::string(text.substr(start, end - start));
}

std::string trim(std::string_view text) {
	size_t begin = 0;
	while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
	size_t end = text.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
	return std::string(text.substr(begin, end - begin));
}

std::string unquote(std::string_view text) {
	auto value = trim(text);
	if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
			(value.front() == '\'' && value.back() == '\''))) {
		return value.substr(1, value.size() - 2);
	}
	return value;
}

} // namespace gdscript_lsp
