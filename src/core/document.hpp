#pragma once

#include "core/types.hpp"
#include <tree_sitter/api.h>

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gdscript_lsp {

struct ClassRecord {
	Symbol symbol;
	std::string extends_text = "RefCounted";
	std::string global_name;
	std::string base_class_id;
	std::string inheritance_error;
	std::vector<Symbol> members;
};

class Document {
public:
	// Deferred documents expose only source/tree/edit data until cloned for a workspace.
	enum class Analysis { Eager, Deferred };
	Document(std::string uri, std::string resource_path, std::string source, int64_t version = -1,
		Analysis analysis = Analysis::Eager);
	Document(std::string uri, std::string resource_path, std::string source, int64_t version,
		const Document &previous, Analysis analysis = Analysis::Eager);
	~Document();
	Document(Document &&) noexcept;
	Document &operator=(Document &&) noexcept;
	Document(const Document &) = delete;
	Document &operator=(const Document &) = delete;

	const std::string &uri() const { return uri_; }
	const std::string &resource_path() const { return resource_path_; }
	const std::string &source() const { return source_; }
	int64_t version() const { return version_; }
	const std::vector<ClassRecord> &classes() const { return classes_; }
	std::vector<ClassRecord> &classes() { return classes_; }
	const std::vector<ParseIssue> &syntax_errors() const { return syntax_errors_; }
	const SyntaxNode &syntax_root() const { return syntax_root_; }
	bool used_incremental_parse() const { return used_incremental_parse_; }
	// Borrowed tree: valid while this document lives. Never edit it in place.
	const TSTree *concrete_tree() const;
	const std::optional<TSInputEdit> &edit() const { return edit_; }
	const std::vector<TSRange> &changed_ranges() const { return changed_ranges_; }
	// Copies the concrete tree without reparsing the document and completes deferred
	// analysis (including bounded error recovery) into independent semantic records.
	std::shared_ptr<Document> clone_for_workspace() const;
	std::string_view text(const SyntaxNode &node) const;

	const ClassRecord *class_at(Position position) const;
	const Symbol *symbol_at(Position position) const;
	const Symbol *find_local(std::string_view name, Position position) const;
	std::vector<const Symbol *> locals_at(Position position) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	std::string uri_;
	std::string resource_path_;
	std::string source_;
	int64_t version_ = -1;
	std::vector<ClassRecord> classes_;
	std::vector<ParseIssue> syntax_errors_;
	SyntaxNode syntax_root_;
	bool used_incremental_parse_ = false;
	std::optional<TSInputEdit> edit_;
	std::vector<TSRange> changed_ranges_;
	bool analyzed_ = false;
	Document(const Document &other, bool);

	void parse(const Document *previous = nullptr);
	void analyze();
};

} // namespace gdscript_lsp
