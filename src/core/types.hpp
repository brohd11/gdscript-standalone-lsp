#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gdscript_lsp {

struct Position {
	uint32_t line = 0;
	uint32_t character = 0; // UTF-16 code units, as required by LSP.
	auto operator<=>(const Position &) const = default;
};

struct Range {
	Position start;
	Position end;
	bool contains(Position position) const {
		return start <= position && position <= end;
	}
};

enum class SymbolKind : uint8_t {
	File = 1,
	Class = 5,
	Method = 6,
	Property = 7,
	Field = 8,
	Constructor = 9,
	Enum = 10,
	Function = 12,
	Variable = 13,
	Constant = 14,
	String = 15,
	Event = 24,
	TypeParameter = 26,
};

enum class TypeKind : uint8_t {
	Unknown,
	Variant,
	Void,
	Builtin,
	NativeClass,
	ScriptClass,
	Enum,
	Callable,
	Signal,
};

struct ResolvedType {
	TypeKind kind = TypeKind::Unknown;
	std::string name;
	std::string symbol_id;
	bool instance = true;
	std::vector<ResolvedType> arguments;
	ResolvedType() = default;
	ResolvedType(TypeKind p_kind, std::string p_name, std::string p_symbol_id = {},
		bool p_instance = true, std::vector<ResolvedType> p_arguments = {});

	bool known() const { return kind != TypeKind::Unknown; }
	std::string display() const;
	static ResolvedType unknown(std::string reason = {});
};

struct Symbol {
	std::string id;
	std::string name;
	std::string qualified_name;
	std::string uri;
	SymbolKind kind = SymbolKind::Variable;
	Range range;
	Range selection_range;
	std::string declared_type;
	std::string initializer;
	std::string detail;
	std::string documentation;
	bool is_static = false;
	bool is_local = false;
	bool is_parameter = false;
	bool is_variadic = false;
	bool is_inferred = false;
	std::vector<Symbol> children;
};

// A compact, parser-independent view of the source tree. Byte offsets make it
// cheap to recover text from Document::source(), while field retains the
// grammar role ("left", "body", "arguments", and so on).
struct SyntaxNode {
	std::string_view kind;
	std::string_view field;
	Range range;
	uint32_t start_byte = 0;
	uint32_t end_byte = 0;
	std::vector<SyntaxNode> children;
};

struct CompletionItem {
	std::string label;
	std::string detail;
	std::string documentation;
	SymbolKind kind = SymbolKind::Variable;
	std::string insert_text;
	std::string filter_text;
};

struct Location {
	std::string uri;
	Range range;
};

enum class DiagnosticSeverity : uint8_t {
	Error = 1,
	Warning = 2,
	Information = 3,
	Hint = 4,
};

struct DiagnosticRelatedInformation {
	Location location;
	std::string message;
};

struct Diagnostic {
	std::string code;
	std::string message;
	Range range;
	DiagnosticSeverity severity = DiagnosticSeverity::Error;
	std::string source = "gdscript-lsp";
	std::vector<DiagnosticRelatedInformation> related_information;
	Diagnostic() = default;
	Diagnostic(std::string p_code, std::string p_message, Range p_range,
		DiagnosticSeverity p_severity = DiagnosticSeverity::Error);
};

std::string_view type_kind_name(TypeKind kind);

} // namespace gdscript_lsp
