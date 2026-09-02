#pragma once

#include "core/types.hpp"

#include <vector>

namespace gdscript_lsp {

class Document;
class Workspace;

class SemanticAnalyzer {
public:
	static std::vector<Diagnostic> run(const Workspace &workspace, const Document &document);
};

} // namespace gdscript_lsp
