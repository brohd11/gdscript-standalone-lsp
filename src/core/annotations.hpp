#pragma once

#include "core/native_api.hpp"
#include "core/types.hpp"
#include "core/warnings.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gdscript_lsp {

class Document;
class Workspace;

enum class AnnotationTarget : uint16_t {
	None = 0,
	Script = 1 << 0,
	Class = 1 << 1,
	Variable = 1 << 2,
	Constant = 1 << 3,
	Signal = 1 << 4,
	Function = 1 << 5,
	Statement = 1 << 6,
	Standalone = 1 << 7,
};

constexpr AnnotationTarget operator|(AnnotationTarget left, AnnotationTarget right) {
	return static_cast<AnnotationTarget>(static_cast<uint16_t>(left) | static_cast<uint16_t>(right));
}
constexpr bool annotation_target_has(AnnotationTarget set, AnnotationTarget value) {
	return (static_cast<uint16_t>(set) & static_cast<uint16_t>(value)) != 0;
}

enum class AnnotationArgumentType : uint8_t { String, Integer, Float };
enum class AnnotationRule : uint8_t {
	None, Tool, Icon, StaticUnload, Abstract, Onready, Export, ExportStorage,
	ExportCustom, ExportToolButton, ExportGroup, WarningIgnore, WarningRegion, Rpc,
};

struct AnnotationArgumentSpec {
	AnnotationArgumentType type = AnnotationArgumentType::String;
	bool has_default = false;
};

struct AnnotationSpec {
	std::string name;
	AnnotationTarget targets = AnnotationTarget::None;
	std::vector<AnnotationArgumentSpec> arguments;
	bool variadic = false;
	bool literal_strings = false;
	AnnotationRule rule = AnnotationRule::None;
};

class AnnotationRegistry {
public:
	const AnnotationSpec *find(std::string_view name) const;
	bool valid_warning(std::string_view name) const;
	bool strict_unknown_names() const { return strict_unknown_names_; }
	GodotVersion target_version() const { return target_version_; }
	GodotVersion newest_known_version() const { return newest_known_version_; }
	size_t size() const { return annotations_.size(); }
	void define(AnnotationSpec spec);
	void erase(std::string_view name);
	void define_warning(std::string name);
	void erase_warning(std::string_view name);

private:
	friend AnnotationRegistry build_annotation_registry(GodotVersion target);
	std::unordered_map<std::string, AnnotationSpec> annotations_;
	std::unordered_set<std::string> warning_names_;
	GodotVersion target_version_;
	GodotVersion newest_known_version_{4, 6, 3};
	bool strict_unknown_names_ = true;
};

AnnotationRegistry build_annotation_registry(GodotVersion target);

struct AnnotationAnalysis {
	std::vector<ParseIssue> issues;
	WarningSuppressions suppressions;
};

class AnnotationAnalyzer {
public:
	static AnnotationAnalysis run(const Workspace &workspace, const Document &document,
		const AnnotationRegistry &registry);
};

} // namespace gdscript_lsp
