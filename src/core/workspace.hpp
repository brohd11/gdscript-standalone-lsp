#pragma once

#include "core/document.hpp"
#include "core/native_api.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gdscript_lsp {

class SemanticAnalyzer;
class SemanticAnalyzerImpl;

enum class WarningLevel : uint8_t {
	Ignore = 0,
	Warning = 1,
	Error = 2,
};

struct HoverResult {
	std::string markdown;
	Range range;
};

struct IndexStats {
	size_t document_count = 0;
	size_t class_count = 0;
	size_t syntax_error_count = 0;
	double elapsed_ms = 0.0;
};

class Workspace {
public:
	bool open(const std::filesystem::path &root, const std::filesystem::path &api_path = {}, std::string *error = nullptr);
	bool update_document(const std::string &uri, std::string text, int64_t version, std::string *error = nullptr);
	bool close_document(const std::string &uri, std::string *error = nullptr);
	bool refresh_file(const std::string &uri, std::string *error = nullptr);

	std::vector<CompletionItem> completion(const std::string &uri, Position position) const;
	CompletionResult completion_result(const std::string &uri, Position position,
		CompletionProfile profile = CompletionProfile::Full) const;
	void set_completion_config(CompletionConfig config);
	CompletionConfig completion_config() const;
	std::optional<HoverResult> hover(const std::string &uri, Position position) const;
	std::vector<Location> definition(const std::string &uri, Position position) const;
	std::vector<Symbol> document_symbols(const std::string &uri) const;
	ResolvedExpression resolve_expression(const std::string &uri, Position position,
		std::string expression = {}) const;
	ResolvedType resolve_type(const std::string &uri, Position position, std::string expression = {}) const;
	std::optional<CompletionItem> resolve_completion_item(std::string_view symbol_id) const;
	std::vector<Diagnostic> diagnostics(const std::string &uri) const;
	std::vector<std::string> document_uris() const;
	// Returns the changed documents and every document whose semantic view can
	// depend on them. Callers that replace a document should query both before
	// and after the replacement, then take the union so removed edges are kept.
	std::vector<std::string> affected_documents(const std::vector<std::string> &changed_uris) const;
	int64_t document_version(const std::string &uri) const;

	const NativeApi &native_api() const { return native_api_; }
	const IndexStats &stats() const { return stats_; }
	const std::filesystem::path &root() const { return root_; }
	std::string uri_for_path(const std::filesystem::path &path) const;
	std::filesystem::path path_for_uri(const std::string &uri) const;

private:
	friend class SemanticAnalyzer;
	friend class SemanticAnalyzerImpl;
	std::filesystem::path root_;
	NativeApi native_api_;
	IndexStats stats_;
	mutable std::shared_mutex mutex_;
	std::unordered_map<std::string, std::shared_ptr<Document>> documents_;
	std::unordered_map<std::string, std::string> disk_sources_;
	std::unordered_map<std::string, ClassRecord *> classes_;
	std::unordered_map<std::string, std::string> global_classes_;
	std::unordered_map<std::string, std::string> autoloads_;
	std::unordered_map<std::string, std::string> uid_paths_;
	std::unordered_map<std::string, size_t> global_name_counts_;
	std::unordered_map<std::string, std::string> symbol_owners_;
	std::unordered_map<std::string, const Symbol *> symbols_;
	mutable std::mutex access_path_cache_mutex_;
	mutable std::unordered_map<std::string, std::vector<AccessPath>> access_path_cache_;
	std::unordered_map<std::string, ResolvedType> static_symbol_types_;
	std::unordered_map<std::string, std::unordered_set<std::string>> document_dependencies_;
	std::unordered_map<std::string, std::unordered_set<std::string>> reverse_document_dependencies_;
	WarningLevel unsafe_property_access_ = WarningLevel::Ignore;
	WarningLevel unsafe_method_access_ = WarningLevel::Ignore;
	WarningLevel unsafe_call_argument_ = WarningLevel::Ignore;
	CompletionConfig completion_config_;

	void rebuild_registry();
	void read_project_settings();
	void scan_uid_files();
	std::string resource_path(const std::filesystem::path &path) const;
	std::string resolve_path_reference(std::string reference, std::string_view owner_resource) const;
	const Document *find_document(const std::string &uri) const;
	const ClassRecord *find_class(std::string_view id) const;
	const Symbol *find_member(const ClassRecord &record, std::string_view name) const;
	std::vector<const Symbol *> all_members(const ClassRecord &record,
		MemberAccess access = MemberAccess::Instance) const;
	ResolvedType resolve_static_reference(std::string expression, const ClassRecord *context,
		std::unordered_set<std::string> &stack) const;
	ResolvedType resolve_static_symbol(const Symbol &symbol, std::unordered_set<std::string> &stack) const;
	std::string native_base(const ClassRecord &record) const;
	ResolvedType type_from_name(std::string name, const ClassRecord *context) const;
	std::optional<std::string> invalid_type_message(std::string_view name, const ClassRecord *context) const;
	std::optional<SymbolOrigin> symbol_origin(std::string_view id) const;
	std::vector<AccessPath> access_paths_for_type(const ResolvedType &type, const ClassRecord *context) const;
	ResolvedType type_for_resource_path(std::string resource_path, const ClassRecord *context) const;
	ResolvedType type_of_symbol(const Symbol &symbol, const Document &document, Position position,
		std::vector<std::string> &stack) const;
	ResolvedType hinted_type_of_symbol(const Symbol &symbol, const Document &document, Position position,
		std::vector<std::string> &stack) const;
	ResolvedType callable_return_type(const Symbol &symbol, const Document &document,
		std::vector<std::string> &stack) const;
	ResolvedType member_value_type(const ResolvedType &receiver, std::string_view member_name,
		const Document &document, Position position, std::vector<std::string> &stack) const;
	ResolvedType infer_expression(std::string expression, const Document &document, const ClassRecord *context,
		Position position, std::vector<std::string> &stack) const;
	bool is_assignable(const ResolvedType &expected, const ResolvedType &actual) const;
	bool is_potential_downcast(const ResolvedType &expected, const ResolvedType &actual) const;
	const ClassRecord *enclosing_class(const ClassRecord &record) const;
	const Symbol *find_lexical_member(const ClassRecord &record, std::string_view name) const;
	const Symbol *resolve_identifier(const Document &document, const ClassRecord *context,
		std::string_view name, Position position) const;
	std::vector<CompletionItem> semantic_completion_locked(const Document &document, Position position) const;
};

} // namespace gdscript_lsp
