#include "core/warnings.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <sstream>
#include <nlohmann/json.hpp>

namespace gdscript_lsp {
namespace {
constexpr std::string_view default_warnings[] = {
	"unused_variable", "unused_local_constant", "unused_parameter",
	"shadowed_variable", "shadowed_variable_base_class", "shadowed_global_identifier",
	"unreachable_code", "unreachable_pattern", "standalone_expression", "standalone_ternary",
	"unsafe_void_return",
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

bool WarningSuppressions::contains(std::string_view name, Position position) const {
	auto key = normalized(name);
	return std::any_of(entries_.begin(), entries_.end(), [&](const Entry &entry) { return entry.name == key && entry.range.contains(position); });
}

void WarningSuppressions::add(std::string name, Range range) {
	entries_.push_back({normalized(name), range});
}
} // namespace gdscript_lsp
