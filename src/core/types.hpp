#pragma once

#include <cstdint>
#include <memory>
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
	// The declaration which produced this value. This is intentionally separate
	// from symbol_id: symbol_id identifies the value's type (or an enum/callable
	// identity), while declaration_id identifies the member/local/native API item
	// rich completion providers should inspect.
	std::string declaration_id;
	bool instance = true;
	std::vector<ResolvedType> arguments;
	// Semantic metadata used while evaluating expressions. It is deliberately
	// omitted from the LSP/GDExtension wire representation: callers still see a
	// plain Callable or Signal, while the resolver can continue through call()
	// and await expressions without losing the value's provenance.
	std::shared_ptr<ResolvedType> callable_return;
	std::vector<ResolvedType> signal_arguments;
	ResolvedType() = default;
	ResolvedType(TypeKind p_kind, std::string p_name, std::string p_symbol_id = {},
		bool p_instance = true, std::vector<ResolvedType> p_arguments = {});

	bool known() const { return kind != TypeKind::Unknown; }
	std::string display() const;
	static ResolvedType unknown(std::string reason = {});
};

struct SymbolOrigin {
	std::string symbol_id;
	std::string uri;
	std::string owner_id;
	std::string name;
	SymbolKind kind = SymbolKind::Variable;
	Range range;
	bool valid = false;
};

enum class AccessPathKind : uint8_t {
	ScriptAlias,
	Local,
	Global,
	Native,
};

struct AccessPath {
	std::string text;
	AccessPathKind kind = AccessPathKind::Local;
	bool preferred = false;
};

struct AccessProvenance {
	std::string declaration_access;
	std::string declaration_context_id;
	std::string receiver_access;
};

struct ResolvedExpression {
	ResolvedType type;
	std::optional<SymbolOrigin> origin;
	std::vector<AccessPath> access_paths;
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
	bool is_iteration_variable = false;
	bool malformed = false;
	// The function needs bounded syntax recovery. This alone must not make an
	// otherwise intact exported signature look malformed.
	bool body_recovered = false;
	std::vector<Symbol> children;
};

bool is_type_level_member(const Symbol &symbol);

// A resolved, presentation-ready document symbol. The standard LSP projection
// uses the common fields, while native and custom clients can consume the
// declaration identity and semantic metadata without issuing one query per
// outline item.
struct OutlineSymbol {
	std::string symbol_id;
	std::string owner_id;
	std::string name;
	std::string qualified_name;
	std::string uri;
	std::string declared_type;
	std::string initializer;
	std::string detail;
	std::string documentation;
	SymbolKind kind = SymbolKind::Variable;
	Range range;
	Range selection_range;
	ResolvedType resolved_type;
	std::optional<ResolvedType> return_type;
	std::optional<SymbolOrigin> origin;
	bool is_static = false;
	bool static_typed = false;
	bool inferred = false;
	bool is_local = false;
	bool is_parameter = false;
	bool is_variadic = false;
	bool is_iteration_variable = false;
	bool malformed = false;
	bool body_recovered = false;
	std::vector<OutlineSymbol> children;
};

struct OutlineSnapshot {
	int64_t version = -1;
	std::vector<OutlineSymbol> symbols;
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
	bool has_error = false;
	std::vector<SyntaxNode> children;
};

struct CompletionItem {
	std::string label;
	std::string detail;
	std::string documentation;
	SymbolKind kind = SymbolKind::Variable;
	std::string insert_text;
	std::string filter_text;
	std::string sort_text;
	std::string symbol_id;
	std::string origin_id;
	std::string provider;
	std::string access_kind;
};

enum class CompletionDisposition : uint8_t {
	NotHandled,
	Augment,
	Replace,
};

enum class CompletionProfile : uint8_t {
	Full,
	Helpers,
};

struct CompletionConfig {
	bool enums = true;
	bool extended_type_hints = true;
	bool constructors = true;
	bool hide_private = true;
	bool member_strings = true;
	bool member_strings_prefer_string_name = true;
	bool member_strings_include_private = false;
};

struct CompletionResult {
	std::vector<CompletionItem> items;
	CompletionDisposition disposition = CompletionDisposition::NotHandled;
	std::string provider;
	bool is_incomplete = false;
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

struct ParseIssue {
	Range range;
	std::string message = "Syntax error.";
};

std::string_view type_kind_name(TypeKind kind);
std::string_view access_path_kind_name(AccessPathKind kind);

} // namespace gdscript_lsp
