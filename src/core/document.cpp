#include "core/document.hpp"
#include "core/text.hpp"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <functional>

extern "C" const TSLanguage *tree_sitter_gdscript(void);

namespace gdscript_lsp {
namespace {

std::string node_type(TSNode node) {
	return ts_node_is_null(node) ? std::string{} : std::string(ts_node_type(node));
}

std::string node_text(TSNode node, std::string_view source) {
	if (ts_node_is_null(node)) return {};
	auto start = static_cast<size_t>(ts_node_start_byte(node));
	auto end = static_cast<size_t>(ts_node_end_byte(node));
	if (start > source.size() || end > source.size() || start > end) return {};
	return std::string(source.substr(start, end - start));
}

TSNode field(TSNode node, const char *name) {
	if (ts_node_is_null(node)) return {};
	return ts_node_child_by_field_name(node, name, static_cast<uint32_t>(std::strlen(name)));
}

Range node_range(TSNode node, std::string_view source) {
	if (ts_node_is_null(node)) return {};
	return {byte_to_position(source, ts_node_start_byte(node)), byte_to_position(source, ts_node_end_byte(node))};
}

size_t utf8_width(unsigned char value) {
	if ((value & 0x80U) == 0) return 1;
	if ((value & 0xE0U) == 0xC0U) return 2;
	if ((value & 0xF0U) == 0xE0U) return 3;
	if ((value & 0xF8U) == 0xF0U) return 4;
	return 1;
}

Position syntax_position(std::string_view source, uint32_t byte, TSPoint point) {
	uint32_t utf16_column = 0;
	auto offset = byte >= point.column ? static_cast<size_t>(byte - point.column) : 0;
	while (offset < byte && offset < source.size()) {
		auto width = std::min(utf8_width(static_cast<unsigned char>(source[offset])), static_cast<size_t>(byte) - offset);
		uint32_t codepoint = static_cast<unsigned char>(source[offset]);
		if (width > 1) {
			codepoint &= (1U << (7U - static_cast<unsigned>(width))) - 1U;
			for (size_t index = 1; index < width; ++index) {
				codepoint = (codepoint << 6U) | (static_cast<unsigned char>(source[offset + index]) & 0x3FU);
			}
		}
		utf16_column += codepoint > 0xFFFFU ? 2U : 1U;
		offset += width;
	}
	return {point.row, utf16_column};
}

Range syntax_range(TSNode node, std::string_view source) {
	auto start_byte = ts_node_start_byte(node);
	auto end_byte = ts_node_end_byte(node);
	return {syntax_position(source, start_byte, ts_node_start_point(node)),
		syntax_position(source, end_byte, ts_node_end_point(node))};
}

SyntaxNode syntax_node(TSNode node, std::string_view source, std::string_view field_name = {}) {
	SyntaxNode result;
	result.kind = ts_node_type(node);
	result.field = field_name;
	result.range = syntax_range(node, source);
	result.start_byte = ts_node_start_byte(node);
	result.end_byte = ts_node_end_byte(node);
	for (uint32_t index = 0; index < ts_node_child_count(node); ++index) {
		auto child = ts_node_child(node, index);
		if (!ts_node_is_named(child)) continue;
		const char *child_field = ts_node_field_name_for_child(node, index);
		result.children.push_back(syntax_node(child, source, child_field ? child_field : ""));
	}
	return result;
}

TSNode first_descendant(TSNode node, std::string_view wanted) {
	if (node_type(node) == wanted) return node;
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		auto found = first_descendant(ts_node_named_child(node, i), wanted);
		if (!ts_node_is_null(found)) return found;
	}
	return {};
}

bool has_named_child(TSNode node, std::string_view wanted) {
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		if (node_type(ts_node_named_child(node, i)) == wanted) return true;
	}
	return false;
}

std::string extends_text(TSNode node, std::string_view source) {
	if (ts_node_is_null(node)) return {};
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		auto child = ts_node_named_child(node, i);
		if (node_type(child) == "annotations") continue;
		return trim(node_text(child, source));
	}
	auto raw = trim(node_text(node, source));
	if (raw.starts_with("extends")) return trim(raw.substr(7));
	return raw;
}

Symbol variable_symbol(TSNode node, const std::string &uri, const std::string &owner_id,
		std::string_view source, bool local) {
	Symbol symbol;
	auto name_node = field(node, "name");
	symbol.name = trim(node_text(name_node, source));
	symbol.id = owner_id + "::" + symbol.name;
	symbol.qualified_name = symbol.id;
	symbol.uri = uri;
	symbol.kind = node_type(node) == "const_statement" ? SymbolKind::Constant : SymbolKind::Variable;
	symbol.range = node_range(node, source);
	symbol.selection_range = node_range(name_node, source);
	symbol.declared_type = trim(node_text(field(node, "type"), source));
	if (symbol.declared_type == ":=") symbol.declared_type.clear();
	symbol.initializer = trim(node_text(field(node, "value"), source));
	symbol.is_static = !ts_node_is_null(field(node, "static")) || has_named_child(node, "static_keyword");
	symbol.is_local = local;
	symbol.detail = (symbol.kind == SymbolKind::Constant ? "const " : "var ") + symbol.name;
	if (!symbol.declared_type.empty()) symbol.detail += ": " + symbol.declared_type;
	return symbol;
}

void collect_locals(TSNode node, Symbol &function, std::string_view source) {
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		auto child = ts_node_named_child(node, i);
		auto type = node_type(child);
		if (type == "variable_statement" || type == "const_statement") {
			function.children.push_back(variable_symbol(child, function.uri, function.id, source, true));
		} else if (type == "for_statement") {
			auto left = field(child, "left");
			Symbol local;
			local.name = trim(node_text(left, source));
			local.id = function.id + "::" + local.name;
			local.qualified_name = local.id;
			local.uri = function.uri;
			local.kind = SymbolKind::Variable;
			local.range = node_range(child, source);
			local.selection_range = node_range(left, source);
			local.declared_type = trim(node_text(field(child, "type"), source));
			if (local.declared_type == ":=") local.declared_type.clear();
			// The iterable is not an initializer for the loop variable. Its element
			// type is inferred by the semantic analyzer instead.
			local.initializer.clear();
			local.is_local = true;
			function.children.push_back(std::move(local));
		}
		collect_locals(child, function, source);
	}
}

Symbol parameter_symbol(TSNode node, const Symbol &function, std::string_view source) {
	Symbol result;
	auto identifier = first_descendant(node, "identifier");
	if (ts_node_is_null(identifier)) identifier = first_descendant(node, "name");
	result.name = trim(node_text(identifier, source));
	result.id = function.id + "::" + result.name;
	result.qualified_name = result.id;
	result.uri = function.uri;
	result.kind = SymbolKind::Variable;
	result.range = node_range(node, source);
	result.selection_range = node_range(identifier, source);
	result.declared_type = trim(node_text(field(node, "type"), source));
	if (result.declared_type.empty()) {
		auto typed = first_descendant(node, "typed_parameter");
		result.declared_type = trim(node_text(field(typed, "type"), source));
	}
	if (result.declared_type == ":=") result.declared_type.clear();
	result.initializer = trim(node_text(field(node, "value"), source));
	result.is_local = true;
	result.is_parameter = true;
	result.is_variadic = node_type(node) == "variadic_parameter" || !ts_node_is_null(first_descendant(node, "variadic_parameter"));
	return result;
}

Symbol function_symbol(TSNode node, const std::string &uri, const std::string &owner_id, std::string_view source) {
	Symbol symbol;
	auto name_node = field(node, "name");
	symbol.name = node_type(node) == "constructor_definition" ? "_init" : trim(node_text(name_node, source));
	if (symbol.name.empty()) symbol.name = "<lambda>";
	symbol.id = owner_id + "::" + symbol.name;
	symbol.qualified_name = symbol.id;
	symbol.uri = uri;
	symbol.kind = symbol.name == "_init" ? SymbolKind::Constructor : SymbolKind::Method;
	symbol.range = node_range(node, source);
	symbol.selection_range = ts_node_is_null(name_node) ? symbol.range : node_range(name_node, source);
	symbol.declared_type = trim(node_text(field(node, "return_type"), source));
	symbol.is_static = has_named_child(node, "static_keyword");
	symbol.detail = (symbol.is_static ? "static func " : "func ") + symbol.name + "(";
	auto parameters = field(node, "parameters");
	for (uint32_t i = 0; i < ts_node_named_child_count(parameters); ++i) {
		auto parameter = parameter_symbol(ts_node_named_child(parameters, i), symbol, source);
		if (parameter.name.empty()) continue;
		if (!symbol.children.empty()) symbol.detail += ", ";
		symbol.detail += parameter.name;
		if (!parameter.declared_type.empty()) symbol.detail += ": " + parameter.declared_type;
		symbol.children.push_back(std::move(parameter));
	}
	symbol.detail += ") -> " + (symbol.declared_type.empty() ? "Variant" : symbol.declared_type);
	collect_locals(field(node, "body"), symbol, source);
	return symbol;
}

void collect_errors(TSNode node, std::string_view source, std::vector<Range> &errors) {
	if (ts_node_is_error(node) || ts_node_is_missing(node)) errors.push_back(node_range(node, source));
	for (uint32_t i = 0; i < ts_node_named_child_count(node); ++i) {
		collect_errors(ts_node_named_child(node, i), source, errors);
	}
}

} // namespace

struct Document::Impl {
	TSParser *parser = nullptr;
	TSTree *tree = nullptr;
	~Impl() {
		if (tree) ts_tree_delete(tree);
		if (parser) ts_parser_delete(parser);
	}
};

Document::Document(std::string uri, std::string resource_path, std::string source, int64_t version) :
		impl_(std::make_unique<Impl>()), uri_(std::move(uri)), resource_path_(std::move(resource_path)),
		source_(std::move(source)), version_(version) {
	parse();
}

Document::~Document() = default;
Document::Document(Document &&) noexcept = default;
Document &Document::operator=(Document &&) noexcept = default;

void Document::parse() {
	impl_->parser = ts_parser_new();
	if (!ts_parser_set_language(impl_->parser, tree_sitter_gdscript())) return;
	impl_->tree = ts_parser_parse_string(impl_->parser, nullptr, source_.data(), static_cast<uint32_t>(source_.size()));
	if (!impl_->tree) return;
	auto root = ts_tree_root_node(impl_->tree);
	syntax_root_ = syntax_node(root, source_);
	collect_errors(root, source_, syntax_errors_);

	ClassRecord root_class;
	root_class.symbol.id = resource_path_;
	root_class.symbol.name = resource_path_.substr(resource_path_.find_last_of('/') + 1);
	if (root_class.symbol.name.ends_with(".gd")) root_class.symbol.name.resize(root_class.symbol.name.size() - 3);
	root_class.symbol.qualified_name = root_class.symbol.id;
	root_class.symbol.uri = uri_;
	root_class.symbol.kind = SymbolKind::Class;
	root_class.symbol.range = {{0, 0}, byte_to_position(source_, source_.size())};
	root_class.symbol.selection_range = {{0, 0}, {0, 0}};

	std::function<void(TSNode, ClassRecord &, const std::string &)> collect_class;
	collect_class = [&](TSNode container, ClassRecord &record, const std::string &owner_id) {
		for (uint32_t i = 0; i < ts_node_named_child_count(container); ++i) {
			auto child = ts_node_named_child(container, i);
			auto type = node_type(child);
			if (type == "class_name_statement") {
				auto name_node = field(child, "name");
				record.global_name = trim(node_text(name_node, source_));
				record.symbol.name = record.global_name;
				record.symbol.selection_range = node_range(name_node, source_);
				auto ext = field(child, "extends");
				if (!ts_node_is_null(ext)) record.extends_text = extends_text(ext, source_);
			} else if (type == "extends_statement") {
				record.extends_text = extends_text(child, source_);
			} else if (type == "variable_statement" || type == "export_variable_statement" ||
					type == "onready_variable_statement" || type == "const_statement") {
				record.members.push_back(variable_symbol(child, uri_, owner_id, source_, false));
			} else if (type == "function_definition" || type == "constructor_definition") {
				record.members.push_back(function_symbol(child, uri_, owner_id, source_));
			} else if (type == "signal_statement" || type == "enum_definition") {
				auto name_node = field(child, "name");
				Symbol symbol;
				symbol.name = trim(node_text(name_node, source_));
				symbol.id = owner_id + "::" + symbol.name;
				symbol.qualified_name = symbol.id;
				symbol.uri = uri_;
				symbol.kind = type == "signal_statement" ? SymbolKind::Event : SymbolKind::Enum;
				symbol.range = node_range(child, source_);
				symbol.selection_range = node_range(name_node, source_);
				symbol.declared_type = type == "signal_statement" ? "Signal" : symbol.name;
				symbol.detail = trim(node_text(child, source_));
				if (type == "enum_definition") {
					auto body = field(child, "body");
					for (uint32_t value_index = 0; value_index < ts_node_named_child_count(body); ++value_index) {
						auto enumerator = ts_node_named_child(body, value_index);
						auto value_node = field(enumerator, "left");
						Symbol value;
						value.name = trim(node_text(value_node, source_));
						value.id = owner_id + "::" + (symbol.name.empty() ? "" : symbol.name + ".") + value.name;
						value.qualified_name = value.id;
						value.uri = uri_;
						value.kind = SymbolKind::Constant;
						value.range = node_range(enumerator, source_);
						value.selection_range = node_range(value_node, source_);
						value.declared_type = "int";
						if (symbol.name.empty()) record.members.push_back(std::move(value));
						else symbol.children.push_back(std::move(value));
					}
				}
				if (!symbol.name.empty()) record.members.push_back(std::move(symbol));
			} else if (type == "class_definition") {
				auto name_node = field(child, "name");
				ClassRecord inner;
				inner.symbol.name = trim(node_text(name_node, source_));
				inner.symbol.id = owner_id + "." + inner.symbol.name;
				inner.symbol.qualified_name = inner.symbol.id;
				inner.symbol.uri = uri_;
				inner.symbol.kind = SymbolKind::Class;
				inner.symbol.range = node_range(child, source_);
				inner.symbol.selection_range = node_range(name_node, source_);
				inner.extends_text = extends_text(field(child, "extends"), source_);
				if (inner.extends_text.empty()) inner.extends_text = "RefCounted";
				collect_class(field(child, "body"), inner, inner.symbol.id);
				Symbol inner_symbol = inner.symbol;
				inner_symbol.declared_type = inner.symbol.id;
				record.members.push_back(std::move(inner_symbol));
				classes_.push_back(std::move(inner));
			}
		}
	};
	collect_class(root, root_class, root_class.symbol.id);
	classes_.insert(classes_.begin(), std::move(root_class));
}

std::string_view Document::text(const SyntaxNode &node) const {
	if (node.start_byte > source_.size() || node.end_byte > source_.size() || node.start_byte > node.end_byte) return {};
	return std::string_view(source_).substr(node.start_byte, node.end_byte - node.start_byte);
}

const ClassRecord *Document::class_at(Position position) const {
	const ClassRecord *best = classes_.empty() ? nullptr : &classes_.front();
	for (const auto &record : classes_) {
		if (record.symbol.range.contains(position) && (!best || record.symbol.range.start >= best->symbol.range.start)) {
			best = &record;
		}
	}
	return best;
}

const Symbol *Document::symbol_at(Position position) const {
	const Symbol *best = nullptr;
	std::function<void(const Symbol &)> visit = [&](const Symbol &symbol) {
		if (!symbol.range.contains(position)) return;
		if (!best || symbol.range.start >= best->range.start) best = &symbol;
		for (const auto &child : symbol.children) visit(child);
	};
	for (const auto &record : classes_) {
		visit(record.symbol);
		for (const auto &member : record.members) visit(member);
	}
	return best;
}

std::vector<const Symbol *> Document::locals_at(Position position) const {
	std::vector<const Symbol *> result;
	for (const auto &record : classes_) {
		if (!record.symbol.range.contains(position)) continue;
		for (const auto &member : record.members) {
			if ((member.kind != SymbolKind::Method && member.kind != SymbolKind::Constructor) || !member.range.contains(position)) continue;
			for (const auto &local : member.children) {
				if (local.range.start <= position) result.push_back(&local);
			}
		}
	}
	return result;
}

const Symbol *Document::find_local(std::string_view name, Position position) const {
	const Symbol *best = nullptr;
	for (auto *local : locals_at(position)) {
		if (local->name == name && (!best || local->range.start >= best->range.start)) best = local;
	}
	return best;
}

} // namespace gdscript_lsp
