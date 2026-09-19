#include "core/source_lexical.hpp"
#include "core/text.hpp"

#include <algorithm>

namespace gdscript_lsp {

SourceLexicalMap::SourceLexicalMap(std::string_view source) : code_(source.size(), true) {
	auto is_quote = [](char c) { return c == '\'' || c == '"'; };
	for (size_t i = 0; i < source.size();) {
		SourceLexicalSpan span;
		span.begin = i;
		if (source[i] == '#') {
			span.kind = SourceLexicalKind::Comment;
			while (i < source.size() && source[i] != '\n' && source[i] != '\r') ++i;
			span.end = i;
		} else {
			size_t quote_at = i;
			if (i + 1 < source.size() && is_quote(source[i + 1])) {
				if (source[i] == 'r' && (i == 0 || !identifier_byte(source[i - 1]))) {
					span.raw = true;
					++quote_at;
				} else if (source[i] == '&' || source[i] == '^') {
					span.kind = source[i] == '&' ? SourceLexicalKind::StringName : SourceLexicalKind::NodePath;
					++quote_at;
				}
			}
			if (!is_quote(source[quote_at])) { ++i; continue; }
			span.quote_begin = quote_at;
			span.quote = source[quote_at];
			span.delimiter_width = quote_at + 2 < source.size() &&
				source[quote_at + 1] == span.quote && source[quote_at + 2] == span.quote ? 3 : 1;
			span.content_begin = quote_at + span.delimiter_width;
			i = span.content_begin;
			while (i < source.size()) {
				// Even raw strings allow quotes/backslashes to be escaped for
				// delimiter recognition. Their value is not decoded here.
				if (source[i] == '\\') {
					++i;
					if (i < source.size() && (!span.raw || source[i] == span.quote || source[i] == '\\')) ++i;
					continue;
				}
				if (source[i] == span.quote && (span.delimiter_width == 1 ||
						(i + 2 < source.size() && source[i + 1] == span.quote && source[i + 2] == span.quote))) {
					span.closed = true;
					break;
				}
				++i;
			}
			span.content_end = i;
			if (span.closed) i += span.delimiter_width;
			span.end = i;
		}
		std::fill(code_.begin() + span.begin, code_.begin() + span.end, false);
		spans_.push_back(span);
	}
}

const SourceLexicalSpan *SourceLexicalMap::span_at(size_t byte) const {
	auto it = std::upper_bound(spans_.begin(), spans_.end(), byte,
		[](size_t at, const SourceLexicalSpan &span) { return at < span.begin; });
	if (it == spans_.begin()) return nullptr;
	--it;
	return byte < it->end ? &*it : nullptr;
}

SourceLexicalKind SourceLexicalMap::kind_at(size_t byte) const {
	auto *span = span_at(byte);
	return span ? span->kind : SourceLexicalKind::Code;
}

const SourceLexicalSpan *SourceLexicalMap::context_span(size_t offset) const {
	if (offset == 0 || offset > code_.size()) return nullptr;
	auto *span = span_at(offset - 1);
	if (!span) return nullptr;
	if (span->kind == SourceLexicalKind::Comment) return span;
	return offset > span->quote_begin && (offset < span->end || !span->closed) ? span : nullptr;
}

SourceLexicalKind SourceLexicalMap::context_at(size_t offset) const {
	auto *span = context_span(offset);
	return span ? span->kind : SourceLexicalKind::Code;
}

std::string SourceLexicalMap::masked_text(std::string_view source, size_t begin, size_t end) const {
	begin = std::min(begin, source.size());
	end = std::clamp(end, begin, source.size());
	std::string result(source.substr(begin, end - begin));
	for (size_t i = begin; i < end && i < code_.size(); ++i) {
		if (!code_[i] && source[i] != '\n' && source[i] != '\r') result[i - begin] = ' ';
	}
	return result;
}

} // namespace gdscript_lsp
