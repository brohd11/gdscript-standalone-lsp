#pragma once

#include "core/types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace gdscript_lsp {

class Document;

enum class WarningLevel : uint8_t { Ignore = 0, Warning = 1, Error = 2 };

// Names use Godot's underscore spelling internally and in project settings.
class WarningPolicy {
public:
	void load(std::string_view project_settings);
	WarningLevel level(std::string_view name, std::string_view resource_path) const;
private:
	bool enabled_ = true;
	std::unordered_map<std::string, WarningLevel> levels_;
	std::unordered_map<std::string, int> directories_{{"res://addons/", 0}};
};

class WarningSuppressions {
public:
	explicit WarningSuppressions(const Document &document);
	bool contains(std::string_view name, Position position) const;
private:
	struct Entry { std::string name; Range range; };
	std::vector<Entry> entries_;
};

} // namespace gdscript_lsp
