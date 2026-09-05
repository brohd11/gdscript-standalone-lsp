#pragma once
#include "core/document.hpp"

namespace gdscript_lsp {
std::vector<ParseIssue> structural_issues(const Document &document);
enum Flow : unsigned { FallsThrough = 1, Returns = 2, Breaks = 4, Continues = 8 };
unsigned statement_flow(const SyntaxNode &node, const Document &document);
bool catch_all_pattern(const SyntaxNode &section, const Document &document);
}
