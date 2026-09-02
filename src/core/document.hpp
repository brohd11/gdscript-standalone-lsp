#pragma once

#include "core/types.hpp"

#include <memory>
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
	Document(std::string uri, std::string resource_path, std::string source, int64_t version = -1);
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
	const std::vector<Range> &syntax_errors() const { return syntax_errors_; }
	const SyntaxNode &syntax_root() const { return syntax_root_; }
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
	std::vector<Range> syntax_errors_;
	SyntaxNode syntax_root_;

	void parse();
};

} // namespace gdscript_lsp
