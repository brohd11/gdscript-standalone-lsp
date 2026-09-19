#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gdscript_lsp {

enum class SourceLexicalKind : uint8_t { Code, Comment, String, StringName, NodePath };

struct SourceLexicalSpan {
	size_t begin = 0;
	size_t end = 0;
	size_t quote_begin = 0;
	size_t content_begin = 0;
	size_t content_end = 0;
	SourceLexicalKind kind = SourceLexicalKind::String;
	char quote = 0;
	uint8_t delimiter_width = 0;
	bool raw = false;
	bool closed = false;
};

// Owns offsets, never views into the input. Safe to share between immutable
// document snapshots, and usable on standalone expressions without a parser.
class SourceLexicalMap {
public:
	explicit SourceLexicalMap(std::string_view source);
	const std::vector<bool> &code() const { return code_; }
	const std::vector<SourceLexicalSpan> &spans() const { return spans_; }
	const SourceLexicalSpan *span_at(size_t byte) const;
	SourceLexicalKind kind_at(size_t byte) const;
	// Context at an insertion boundary, rather than classification of its byte.
	const SourceLexicalSpan *context_span(size_t offset) const;
	SourceLexicalKind context_at(size_t offset) const;
	std::string masked_text(std::string_view source, size_t begin, size_t end) const;

private:
	std::vector<bool> code_;
	std::vector<SourceLexicalSpan> spans_;
};

} // namespace gdscript_lsp
