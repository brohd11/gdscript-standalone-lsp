#include "core/warnings.hpp"

#include "core/document.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <sstream>
#include <nlohmann/json.hpp>

namespace gdscript_lsp {
namespace {
constexpr std::string_view default_warnings[] = {
	"unused_variable", "unused_local_constant", "unused_parameter",
	"shadowed_variable", "shadowed_variable_base_class", "shadowed_global_identifier",
	"unreachable_code", "unreachable_pattern", "unsafe_void_return",
};
std::string normalized(std::string_view name) {
	std::string result(name);
	std::replace(result.begin(), result.end(), '-', '_');
	return result;
}
}

void WarningPolicy::load(std::string_view settings) {
	enabled_ = true;
	levels_.clear();
	directories_ = {{"res://addons/", 0}};
	for (auto name : default_warnings) levels_[std::string(name)] = WarningLevel::Warning;
	std::istringstream stream{std::string(settings)};
	std::string line, section;
	while (std::getline(stream, line)) {
		auto clean = trim(line);
		if (clean.starts_with('[') && clean.ends_with(']')) { section = clean.substr(1, clean.size() - 2); continue; }
		if (section != "debug") continue;
		auto split = clean.find('=');
		if (split == std::string::npos) continue;
		auto key = trim(clean.substr(0, split));
		if (!key.starts_with("gdscript/warnings/")) continue;
		key.erase(0, 18);
		auto value = trim(clean.substr(split + 1));
		if (key == "directory_rules") {
			while (value.find('}') == std::string::npos && std::getline(stream, line)) value += line;
			auto object = nlohmann::json::parse(value, nullptr, false);
			if (!object.is_object()) continue;
			directories_.clear();
			for (auto entry = object.begin(); entry != object.end(); ++entry) {
				if (!entry.value().is_number_integer() || !entry.key().starts_with("res://")) continue;
				auto path = entry.key();
				if (!path.ends_with('/')) path += '/';
				directories_[path] = entry.value().get<int>();
			}
			continue;
		}
		if (auto comment = value.find(';'); comment != std::string::npos) value = trim(value.substr(0, comment));
		if (key == "enable") { enabled_ = value != "false"; continue; }
		if (value == "0" || value == "1" || value == "2") levels_[key] = static_cast<WarningLevel>(value[0] - '0');
	}
}

WarningLevel WarningPolicy::level(std::string_view name, std::string_view path) const {
	if (!enabled_) return WarningLevel::Ignore;
	size_t longest = 0;
	bool included = true;
	for (const auto &[directory, decision] : directories_) {
		if ((decision == 0 || decision == 1) && path.starts_with(directory) && directory.size() > longest) {
			longest = directory.size(); included = decision == 1;
		}
	}
	if (!included) return WarningLevel::Ignore;
	auto found = levels_.find(normalized(name));
	return found == levels_.end() ? WarningLevel::Ignore : found->second;
}

WarningSuppressions::WarningSuppressions(const Document &document) {
	struct Annotation { const SyntaxNode *node; Range target; };
	std::vector<Annotation> annotations;
	struct Constant { std::string name; const SyntaxNode *value; Range scope; Position position; unsigned depth; };
	std::vector<Constant> constants;
	std::function<void(const SyntaxNode &, Range, unsigned)> collect = [&](const SyntaxNode &node, Range scope, unsigned depth) {
		if (node.kind == "body") { scope = node.range; ++depth; }
		if (node.kind == "const_statement") {
			const SyntaxNode *name = nullptr, *value = nullptr;
			for (const auto &child : node.children) {
				if (child.field == "name") name = &child;
				if (child.field == "value") value = &child;
			}
			if (name && value) constants.push_back({std::string(document.text(*name)), value, scope, node.range.start, depth});
		}
		for (size_t i = 0; i < node.children.size(); ++i) {
			const auto &child = node.children[i];
			if (child.kind == "annotation") {
				Range target = node.range;
				if (node.kind == "body" || node.kind == "source" || node.kind == "match_body" || node.kind == "annotations") {
					if (node.kind == "annotations") continue; // Collected with the declaration's range below.
					target = child.range;
					for (size_t next = i + 1; next < node.children.size(); ++next) {
						if (node.children[next].kind != "annotation" && node.children[next].kind != "comment") {
							target = node.children[next].range; break;
						}
					}
				}
				annotations.push_back({&child, target});
			} else if (child.kind == "annotations") {
				for (const auto &annotation : child.children) if (annotation.kind == "annotation") annotations.push_back({&annotation, node.range});
			} else collect(child, scope, depth);
		}
	};
	collect(document.syntax_root(), document.syntax_root().range, 0);
	std::function<std::string(const SyntaxNode &, Position, unsigned)> constant_string = [&](const SyntaxNode &node, Position use, unsigned recursion) -> std::string {
		if (recursion > 32) return {};
		if (node.kind == "string") return unquote(document.text(node));
		if (node.kind == "parenthesized_expression" && !node.children.empty()) return constant_string(node.children.front(), use, recursion + 1);
		if (node.kind == "identifier") {
			const Constant *best = nullptr;
			for (const auto &constant : constants) if (constant.name == document.text(node) && constant.scope.contains(use) &&
				(constant.depth == 0 || constant.position < use) && (!best || constant.depth > best->depth)) best = &constant;
			if (best) return constant_string(*best->value, best->position, recursion + 1);
		}
		if (node.kind == "binary_operator") {
			const SyntaxNode *left = nullptr, *right = nullptr;
			for (const auto &child : node.children) {
				if (child.field == "left") left = &child;
				if (child.field == "right") right = &child;
			}
			if (left && right && trim(std::string_view(document.source()).substr(left->end_byte, right->start_byte - left->end_byte)) == "+") {
				auto a = constant_string(*left, use, recursion + 1), b = constant_string(*right, use, recursion + 1);
				if (!a.empty() && !b.empty()) return a + b;
			}
		}
		return {};
	};
	std::sort(annotations.begin(), annotations.end(), [](const auto &a, const auto &b) { return a.node->start_byte < b.node->start_byte; });
	std::unordered_map<std::string, Position> active;
	for (const auto &annotation : annotations) {
		std::string kind;
		const SyntaxNode *arguments = nullptr;
		for (const auto &child : annotation.node->children) {
			if (child.kind == "identifier") kind = document.text(child);
			if (child.field == "arguments") arguments = &child;
		}
		if (!arguments || (kind != "warning_ignore" && kind != "warning_ignore_start" && kind != "warning_ignore_restore")) continue;
		for (const auto &argument : arguments->children) {
			std::string name;
			if (argument.kind == "string") name = unquote(document.text(argument));
			else if (kind == "warning_ignore") name = constant_string(argument, annotation.node->range.start, 0);
			if (name.empty()) continue;
			name = normalized(name);
			if (kind == "warning_ignore") entries_.push_back({name, annotation.target});
			else if (kind == "warning_ignore_start") active.try_emplace(name, annotation.node->range.end);
			else if (auto found = active.find(name); found != active.end()) {
				entries_.push_back({name, {found->second, annotation.node->range.start}}); active.erase(found);
			}
		}
	}
	for (const auto &[name, start] : active) entries_.push_back({name, {start, {std::numeric_limits<uint32_t>::max(), 0}}});
}

bool WarningSuppressions::contains(std::string_view name, Position position) const {
	auto key = normalized(name);
	return std::any_of(entries_.begin(), entries_.end(), [&](const Entry &entry) { return entry.name == key && entry.range.contains(position); });
}
} // namespace gdscript_lsp
