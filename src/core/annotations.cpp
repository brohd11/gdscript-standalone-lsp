#include "core/annotations.hpp"

#include "core/document.hpp"
#include "core/text.hpp"
#include "core/workspace.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <variant>

namespace gdscript_lsp {
namespace {

using Target = AnnotationTarget;
using Arg = AnnotationArgumentSpec;

constexpr Target class_level = Target::Class | Target::Variable | Target::Constant |
	Target::Signal | Target::Function;

AnnotationSpec spec(std::string name, Target targets, std::initializer_list<Arg> arguments = {},
		bool variadic = false, AnnotationRule rule = AnnotationRule::None, bool literal_strings = false) {
	return {std::move(name), targets, arguments, variadic, literal_strings, rule};
}

void add_4_6_3_annotations(AnnotationRegistry &registry) {
	auto add = [&](AnnotationSpec value) { registry.define(std::move(value)); };
	const Arg string{AnnotationArgumentType::String, false};
	const Arg optional_string{AnnotationArgumentType::String, true};
	const Arg integer{AnnotationArgumentType::Integer, false};
	const Arg optional_integer{AnnotationArgumentType::Integer, true};
	const Arg number{AnnotationArgumentType::Float, false};
	const Arg optional_number{AnnotationArgumentType::Float, true};

	add(spec("tool", Target::Script, {}, false, AnnotationRule::Tool));
	add(spec("icon", Target::Script, {string}, false, AnnotationRule::Icon, true));
	add(spec("static_unload", Target::Script, {}, false, AnnotationRule::StaticUnload));
	add(spec("abstract", Target::Script | Target::Class | Target::Function, {}, false, AnnotationRule::Abstract));
	add(spec("onready", Target::Variable, {}, false, AnnotationRule::Onready));

	add(spec("export", Target::Variable, {}, false, AnnotationRule::Export));
	add(spec("export_enum", Target::Variable, {string}, true, AnnotationRule::Export));
	add(spec("export_file", Target::Variable, {optional_string}, true, AnnotationRule::Export));
	add(spec("export_file_path", Target::Variable, {optional_string}, true, AnnotationRule::Export));
	add(spec("export_dir", Target::Variable, {}, false, AnnotationRule::Export));
	add(spec("export_global_file", Target::Variable, {optional_string}, true, AnnotationRule::Export));
	add(spec("export_global_dir", Target::Variable, {}, false, AnnotationRule::Export));
	add(spec("export_multiline", Target::Variable, {optional_string}, true, AnnotationRule::Export));
	add(spec("export_placeholder", Target::Variable, {string}, false, AnnotationRule::Export));
	add(spec("export_range", Target::Variable, {number, number, optional_number, optional_string}, true, AnnotationRule::Export));
	add(spec("export_exp_easing", Target::Variable, {optional_string}, true, AnnotationRule::Export));
	add(spec("export_color_no_alpha", Target::Variable, {}, false, AnnotationRule::Export));
	add(spec("export_node_path", Target::Variable, {optional_string}, true, AnnotationRule::Export));
	add(spec("export_flags", Target::Variable, {string}, true, AnnotationRule::Export));
	for (auto name : {"export_flags_2d_render", "export_flags_2d_physics", "export_flags_2d_navigation",
			"export_flags_3d_render", "export_flags_3d_physics", "export_flags_3d_navigation", "export_flags_avoidance"}) {
		add(spec(name, Target::Variable, {}, false, AnnotationRule::Export));
	}
	add(spec("export_storage", Target::Variable, {}, false, AnnotationRule::ExportStorage));
	add(spec("export_custom", Target::Variable, {integer, string, optional_integer}, false, AnnotationRule::ExportCustom));
	add(spec("export_tool_button", Target::Variable, {string, optional_string}, false, AnnotationRule::ExportToolButton));
	add(spec("export_category", Target::Standalone, {string}, false, AnnotationRule::ExportGroup));
	add(spec("export_group", Target::Standalone, {string, optional_string}, false, AnnotationRule::ExportGroup));
	add(spec("export_subgroup", Target::Standalone, {string, optional_string}, false, AnnotationRule::ExportGroup));

	add(spec("warning_ignore", class_level | Target::Statement, {string}, true, AnnotationRule::WarningIgnore));
	add(spec("warning_ignore_start", Target::Standalone, {string}, true, AnnotationRule::WarningRegion, true));
	add(spec("warning_ignore_restore", Target::Standalone, {string}, true, AnnotationRule::WarningRegion, true));
	add(spec("rpc", Target::Function, {optional_string, optional_string, optional_string, optional_integer},
		false, AnnotationRule::Rpc));

	for (auto name : {
		"unassigned_variable", "unassigned_variable_op_assign", "unused_variable", "unused_local_constant",
		"unused_private_class_variable", "unused_parameter", "unused_signal", "shadowed_variable",
		"shadowed_variable_base_class", "shadowed_global_identifier", "unreachable_code", "unreachable_pattern",
		"standalone_expression", "standalone_ternary", "incompatible_ternary", "untyped_declaration",
		"inferred_declaration", "unsafe_property_access", "unsafe_method_access", "unsafe_cast",
		"unsafe_call_argument", "unsafe_void_return", "return_value_discarded", "static_called_on_instance",
		"missing_tool", "redundant_static_unload", "redundant_await", "missing_await", "assert_always_true",
		"assert_always_false", "integer_division", "narrowing_conversion", "int_as_enum_without_cast",
		"int_as_enum_without_match", "enum_variable_without_default", "confusable_identifier",
		"confusable_local_declaration", "confusable_local_usage", "confusable_capture_reassignment",
		"confusable_temporary_modification", "inference_on_variant", "native_method_override",
		"get_node_default_without_onready", "onready_with_export", "empty_file", "deprecated_keyword",
		"property_used_as_function", "constant_used_as_function", "function_used_as_property"}) {
		registry.define_warning(name);
	}
}

} // namespace

const AnnotationSpec *AnnotationRegistry::find(std::string_view name) const {
	auto found = annotations_.find(std::string(name));
	return found == annotations_.end() ? nullptr : &found->second;
}

void AnnotationRegistry::define(AnnotationSpec value) {
	annotations_.insert_or_assign(value.name, std::move(value));
}

void AnnotationRegistry::erase(std::string_view name) {
	annotations_.erase(std::string(name));
}

void AnnotationRegistry::define_warning(std::string name) {
	warning_names_.insert(std::move(name));
}

void AnnotationRegistry::erase_warning(std::string_view name) {
	warning_names_.erase(std::string(name));
}

bool AnnotationRegistry::valid_warning(std::string_view name) const {
	return warning_names_.contains(std::string(name));
}

AnnotationRegistry build_annotation_registry(GodotVersion target) {
	AnnotationRegistry result;
	if (!target.known()) target = {4, 6, 3};
	result.target_version_ = target;
	add_4_6_3_annotations(result);

	// Add forward deltas here in ascending order. A delta may add, erase, or
	// replace both annotation specifications and warning names.
	// if (target >= GodotVersion{4, 7, 0}) apply_4_7_0_annotations(result);

	result.strict_unknown_names_ = target.major == 4 && target.minor == 6;
	return result;
}

namespace {

const SyntaxNode *field(const SyntaxNode &node, std::string_view name) {
	for (const auto &child : node.children) if (child.field == name) return &child;
	return nullptr;
}

std::string lowercase(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

AnnotationTarget declaration_target(const SyntaxNode &node, bool class_scope) {
	if (!class_scope) return AnnotationTarget::Statement;
	if (node.kind == "variable_statement") return AnnotationTarget::Variable;
	if (node.kind == "const_statement") return AnnotationTarget::Constant;
	if (node.kind == "signal_statement") return AnnotationTarget::Signal;
	if (node.kind == "function_definition" || node.kind == "constructor_definition") return AnnotationTarget::Function;
	if (node.kind == "class_definition") return AnnotationTarget::Class;
	if (node.kind == "class_name_statement") return AnnotationTarget::Script;
	return AnnotationTarget::None;
}

std::string target_label(const SyntaxNode *node, bool class_scope) {
	if (!node) return {};
	if (node->kind == "variable_statement") return "variable";
	if (node->kind == "const_statement") return "constant";
	if (node->kind == "signal_statement") return "signal";
	if (node->kind == "function_definition" || node->kind == "constructor_definition") return "function";
	if (node->kind == "class_definition") return "class";
	if (node->kind == "enum_definition") return "enum";
	return class_scope ? "class member" : "statement";
}

std::optional<int64_t> integer_literal(std::string text) {
	text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
	int base = 10;
	size_t offset = 0;
	if (text.starts_with("0x") || text.starts_with("0X")) { base = 16; offset = 2; }
	else if (text.starts_with("0o") || text.starts_with("0O")) { base = 8; offset = 2; }
	else if (text.starts_with("0b") || text.starts_with("0B")) { base = 2; offset = 2; }
	int64_t value = 0;
	auto [end, error] = std::from_chars(text.data() + offset, text.data() + text.size(), value, base);
	if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
	return value;
}

} // namespace

class AnnotationAnalyzerImpl {
public:
	AnnotationAnalyzerImpl(const Workspace &p_workspace, const Document &p_document,
			const AnnotationRegistry &p_registry) :
			workspace(p_workspace), document(p_document), registry(p_registry) {}

	AnnotationAnalysis run() {
		collect_constants(document.syntax_root(), document.syntax_root().range, 0, false);
		collect(document.syntax_root(), true, true);
		std::sort(uses.begin(), uses.end(), [](const Use &left, const Use &right) {
			return left.annotation->start_byte < right.annotation->start_byte;
		});
		for (auto &use : uses) validate(use);
		validate_abstract_classes();
		for (const auto &[name, start] : warning_regions) {
			analysis.suppressions.add(name, {start, {std::numeric_limits<uint32_t>::max(), 0}});
		}
		return std::move(analysis);
	}

private:
	struct Use {
		const SyntaxNode *annotation = nullptr;
		const SyntaxNode *name_node = nullptr;
		const SyntaxNode *target_node = nullptr;
		std::vector<const SyntaxNode *> arguments;
		std::string name;
		std::string target_name;
		AnnotationTarget level = AnnotationTarget::None;
		AnnotationTarget target = AnnotationTarget::None;
		bool class_scope = false;
		bool script_position = false;
	};

	enum class ConstantStatus { Value, NonConstant, Unknown };
	struct ConstantValue {
		ConstantStatus status = ConstantStatus::Unknown;
		std::variant<std::monostate, bool, int64_t, double, std::string> value;
		std::string type;
	};
	struct ConstantDeclaration {
		std::string name;
		const SyntaxNode *value;
		Range scope;
		Position position;
		unsigned depth = 0;
		bool require_before = false;
	};

	const Workspace &workspace;
	const Document &document;
	const AnnotationRegistry &registry;
	AnnotationAnalysis analysis;
	std::vector<Use> uses;
	std::vector<ConstantDeclaration> constants;
	std::set<const SyntaxNode *> reducing;
	std::unordered_map<std::string, Position> warning_regions;
	std::unordered_map<const SyntaxNode *, std::unordered_set<std::string>> applied;
	std::unordered_set<const ClassRecord *> abstract_classes;
	std::vector<std::pair<const ClassRecord *, const SyntaxNode *>> abstract_functions;
	bool tool_script = false;
	bool source_preamble = true;
	bool root_member = false;

	void issue(const SyntaxNode &node, std::string message) {
		analysis.issues.push_back({node.range, std::move(message)});
	}

	void collect_constants(const SyntaxNode &node, Range scope, unsigned depth, bool local) {
		if (node.kind == "class_body" || node.kind == "body") {
			scope = node.range;
			++depth;
			if (node.kind == "body") local = true;
		}
		if (node.kind == "const_statement") {
			auto *name = field(node, "name"), *value = field(node, "value");
			if (name && value) constants.push_back({std::string(document.text(*name)), value, scope,
				node.range.start, depth, local});
		}
		for (const auto &child : node.children) collect_constants(child, scope, depth, local);
	}

	const SyntaxNode *next_target(const SyntaxNode &container, size_t index) const {
		for (++index; index < container.children.size(); ++index) {
			const auto &candidate = container.children[index];
			if (candidate.kind != "annotation") return &candidate;
		}
		return nullptr;
	}

	bool followed_by_script_annotation(const SyntaxNode &container, size_t index) const {
		for (++index; index < container.children.size() && container.children[index].kind == "annotation"; ++index) {
			std::string name;
			for (const auto &child : container.children[index].children) if (child.kind == "identifier") name = document.text(child);
			auto *candidate = registry.find(name);
			if (candidate && annotation_target_has(candidate->targets, AnnotationTarget::Script) &&
					!annotation_target_has(candidate->targets, AnnotationTarget::Class) &&
					!annotation_target_has(candidate->targets, AnnotationTarget::Function)) return true;
		}
		return false;
	}

	Use make_use(const SyntaxNode &annotation, const SyntaxNode *target_node, bool class_scope,
			AnnotationTarget level, AnnotationTarget target, bool script_position) const {
		Use use;
		use.annotation = &annotation;
		use.target_node = target_node;
		use.class_scope = class_scope;
		use.level = level;
		use.target = target;
		use.script_position = script_position;
		use.target_name = target_label(target_node, class_scope);
		for (const auto &child : annotation.children) {
			if (child.kind == "identifier") { use.name_node = &child; use.name = document.text(child); }
			if (child.field == "arguments") for (const auto &argument : child.children) use.arguments.push_back(&argument);
		}
		return use;
	}

	void collect(const SyntaxNode &node, bool class_scope, bool root) {
		const bool container = node.kind == "source" || node.kind == "class_body" ||
			node.kind == "body" || node.kind == "match_body";
		if (container) {
			uint32_t root_barrier = std::numeric_limits<uint32_t>::max();
			if (root) for (const auto &child : node.children) if (child.kind != "annotation") {
				root_barrier = child.start_byte; break;
			}
			for (size_t index = 0; index < node.children.size(); ++index) {
				const auto &child = node.children[index];
				if (child.kind == "annotation") {
					std::string name;
					for (const auto &part : child.children) if (part.kind == "identifier") name = document.text(part);
					auto *definition = registry.find(name);
					const bool at_script_top = root && child.start_byte < root_barrier;
					AnnotationTarget level = class_scope ? class_level | AnnotationTarget::Standalone :
						AnnotationTarget::Statement | AnnotationTarget::Standalone;
					if (root && at_script_top) level = level | AnnotationTarget::Script;
					const SyntaxNode *target_node = next_target(node, index);
					AnnotationTarget target = target_node ? declaration_target(*target_node, class_scope) : AnnotationTarget::None;
					if (definition && annotation_target_has(definition->targets, AnnotationTarget::Standalone)) {
						target_node = nullptr; target = AnnotationTarget::Standalone;
					} else if (definition && annotation_target_has(definition->targets, AnnotationTarget::Script)) {
						const bool pure_script = !annotation_target_has(definition->targets, AnnotationTarget::Class) &&
							!annotation_target_has(definition->targets, AnnotationTarget::Function);
						if (pure_script || (at_script_top && (followed_by_script_annotation(node, index) || !target_node ||
								target_node->kind == "extends_statement" || target_node->kind == "class_name_statement"))) {
							target_node = root ? &node : nullptr; target = AnnotationTarget::Script;
						}
					}
					uses.push_back(make_use(child, target_node, class_scope, level, target, at_script_top));
					continue;
				}
				if (root) {
					auto previous_root_member = root_member;
					root_member = true;
					collect(child, true, false);
					root_member = previous_root_member;
					source_preamble = false;
				} else collect(child, node.kind == "class_body", false);
			}
			return;
		}

		for (const auto &child : node.children) if (child.kind == "annotations") {
			AnnotationTarget target = declaration_target(node, class_scope);
			AnnotationTarget level = class_scope ? class_level | AnnotationTarget::Standalone :
				AnnotationTarget::Statement | AnnotationTarget::Standalone;
			for (const auto &annotation : child.children) if (annotation.kind == "annotation") {
				std::string name;
				for (const auto &part : annotation.children) if (part.kind == "identifier") name = document.text(part);
				auto *definition = registry.find(name);
				const bool script_position = root_member && source_preamble;
				auto annotation_level = script_position ? level | AnnotationTarget::Script : level;
				auto annotation_target = target;
				const SyntaxNode *target_node = &node;
				if (definition && annotation_target_has(definition->targets, AnnotationTarget::Script)) {
					const bool pure_script = !annotation_target_has(definition->targets, AnnotationTarget::Class) &&
						!annotation_target_has(definition->targets, AnnotationTarget::Function);
					if (script_position && (pure_script || node.kind == "class_name_statement")) {
						annotation_target = AnnotationTarget::Script;
						target_node = &document.syntax_root();
					}
				}
				uses.push_back(make_use(annotation, target_node, class_scope, annotation_level,
					annotation_target, script_position));
			}
		}
		for (const auto &child : node.children) {
			if (child.kind == "annotations" || child.kind == "annotation") continue;
			if (child.kind == "class_body") collect(child, true, false);
			else if (child.kind == "body" || child.kind == "match_body") collect(child, false, false);
			else collect(child, class_scope, false);
		}
	}

	ConstantValue reduce(const SyntaxNode &node) {
		if (node.has_error) return {};
		if (node.kind == "string" || node.kind == "string_name" || node.kind == "node_path") {
			return {ConstantStatus::Value, unquote(document.text(node)), "String"};
		}
		if (node.kind == "integer") {
			auto value = integer_literal(std::string(document.text(node)));
			return value ? ConstantValue{ConstantStatus::Value, *value, "int"} : ConstantValue{};
		}
		if (node.kind == "float") {
			auto text = std::string(document.text(node));
			text.erase(std::remove(text.begin(), text.end(), '_'), text.end());
			char *end = nullptr;
			double value = std::strtod(text.c_str(), &end);
			return end == text.c_str() + text.size() ? ConstantValue{ConstantStatus::Value, value, "float"} : ConstantValue{};
		}
		if (node.kind == "true") return {ConstantStatus::Value, true, "bool"};
		if (node.kind == "false") return {ConstantStatus::Value, false, "bool"};
		if (node.kind == "parenthesized_expression" && !node.children.empty()) return reduce(node.children.front());
		if (node.kind == "identifier" || node.kind == "name") {
			auto name = std::string(document.text(node));
			if (auto native = workspace.native_api_.global_enum_value(name)) {
				return {ConstantStatus::Value, *native, "int"};
			}
			const ConstantDeclaration *best = nullptr;
			for (const auto &constant : constants) if (constant.name == name && constant.scope.contains(node.range.start) &&
					(!constant.require_before || constant.position < node.range.start) &&
					(!best || constant.depth > best->depth ||
						(constant.depth == best->depth && constant.position > best->position))) best = &constant;
			if (best) {
				if (!reducing.insert(best->value).second) return {};
				auto value = reduce(*best->value);
				reducing.erase(best->value);
				return value;
			}
			if (auto *symbol = workspace.resolve_identifier(document, document.class_at(node.range.start), name, node.range.start)) {
				return symbol->kind == SymbolKind::Constant ? ConstantValue{} :
					ConstantValue{ConstantStatus::NonConstant, {}, {}};
			}
			return {};
		}
		if (node.kind == "attribute") {
			for (auto iterator = node.children.rbegin(); iterator != node.children.rend(); ++iterator) {
				if (iterator->kind != "identifier") continue;
				if (auto native = workspace.native_api_.global_enum_value(document.text(*iterator))) {
					return {ConstantStatus::Value, *native, "int"};
				}
				break;
			}
			return {};
		}
		if (node.kind == "unary_operator" && !node.children.empty()) {
			auto value = reduce(node.children.back());
			if (value.status != ConstantStatus::Value) return value;
			auto raw = trim(std::string_view(document.source()).substr(node.start_byte,
				node.children.back().start_byte - node.start_byte));
			if (auto *integer = std::get_if<int64_t>(&value.value)) {
				if (raw == "-") *integer = -*integer;
				else if (raw == "~") *integer = ~*integer;
				else if (raw != "+") return {};
				return value;
			}
			if (auto *number = std::get_if<double>(&value.value)) {
				if (raw == "-") *number = -*number;
				else if (raw != "+") return {};
				return value;
			}
			return {};
		}
		if (node.kind == "binary_operator") {
			auto *left_node = field(node, "left"), *right_node = field(node, "right");
			if (!left_node || !right_node) return {};
			auto left = reduce(*left_node), right = reduce(*right_node);
			if (left.status != ConstantStatus::Value || right.status != ConstantStatus::Value) {
				if (left.status == ConstantStatus::NonConstant || right.status == ConstantStatus::NonConstant)
					return {ConstantStatus::NonConstant, {}, {}};
				return {};
			}
			auto op = trim(std::string_view(document.source()).substr(left_node->end_byte,
				right_node->start_byte - left_node->end_byte));
			if (op == "+" && std::holds_alternative<std::string>(left.value) && std::holds_alternative<std::string>(right.value)) {
				return {ConstantStatus::Value, std::get<std::string>(left.value) + std::get<std::string>(right.value), "String"};
			}
			auto integer = [&](const ConstantValue &value) -> std::optional<int64_t> {
				if (auto *result = std::get_if<int64_t>(&value.value)) return *result;
				if (auto *result = std::get_if<bool>(&value.value)) return *result ? 1 : 0;
				return std::nullopt;
			};
			auto number = [&](const ConstantValue &value) -> std::optional<double> {
				if (auto *result = std::get_if<double>(&value.value)) return *result;
				if (auto result = integer(value)) return static_cast<double>(*result);
				return std::nullopt;
			};
			if (auto a = integer(left), b = integer(right); a && b && op != "/" && op != "**") {
				if (op == "+") return {ConstantStatus::Value, *a + *b, "int"};
				if (op == "-") return {ConstantStatus::Value, *a - *b, "int"};
				if (op == "*") return {ConstantStatus::Value, *a * *b, "int"};
				if (op == "%" && *b) return {ConstantStatus::Value, *a % *b, "int"};
				if (op == "|") return {ConstantStatus::Value, *a | *b, "int"};
				if (op == "&") return {ConstantStatus::Value, *a & *b, "int"};
				if (op == "^") return {ConstantStatus::Value, *a ^ *b, "int"};
				if (op == "<<") return {ConstantStatus::Value, *a << *b, "int"};
				if (op == ">>") return {ConstantStatus::Value, *a >> *b, "int"};
			}
			if (auto a = number(left), b = number(right); a && b) {
				if (op == "+") return {ConstantStatus::Value, *a + *b, "float"};
				if (op == "-") return {ConstantStatus::Value, *a - *b, "float"};
				if (op == "*") return {ConstantStatus::Value, *a * *b, "float"};
				if (op == "/" && *b != 0.0) return {ConstantStatus::Value, *a / *b, "float"};
				if (op == "**") return {ConstantStatus::Value, std::pow(*a, *b), "float"};
			}
			return {};
		}
		if (node.kind == "call" || node.kind == "attribute_call" || node.kind == "await_expression") {
			return {ConstantStatus::NonConstant, {}, {}};
		}
		return {};
	}

	static std::string expected_type(AnnotationArgumentType type) {
		if (type == AnnotationArgumentType::String) return "String";
		if (type == AnnotationArgumentType::Integer) return "int";
		return "float";
	}

	bool convertible(const ConstantValue &value, AnnotationArgumentType expected) const {
		if (expected == AnnotationArgumentType::String) return value.type == "String";
		return value.type == "int" || value.type == "float" || value.type == "bool";
	}

	std::vector<ConstantValue> validate_arguments(const Use &use, const AnnotationSpec &definition, bool &valid) {
		valid = true;
		size_t minimum = definition.arguments.size();
		while (minimum && definition.arguments[minimum - 1].has_default) --minimum;
		if (!definition.variadic && use.arguments.size() > definition.arguments.size()) {
			issue(*use.annotation, "Annotation \"@" + use.name + "\" requires at most " +
				std::to_string(definition.arguments.size()) + " arguments, but " +
				std::to_string(use.arguments.size()) + " were given.");
			valid = false; return {};
		}
		if (use.arguments.size() < minimum) {
			issue(*use.annotation, "Annotation \"@" + use.name + "\" requires at least " +
				std::to_string(minimum) + " arguments, but " + std::to_string(use.arguments.size()) + " were given.");
			valid = false; return {};
		}
		std::vector<ConstantValue> values;
		for (size_t index = 0; index < use.arguments.size(); ++index) {
			auto *argument = use.arguments[index];
			if (definition.arguments.empty()) break;
			const auto &argument_spec = definition.arguments[std::min(index, definition.arguments.size() - 1)];
			if (definition.literal_strings && argument->kind != "string") {
				issue(*argument, "Argument " + std::to_string(index + 1) + " of annotation \"@" + use.name +
					"\" must be a string literal.");
				valid = false; return {};
			}
			auto value = reduce(*argument);
			if (value.status == ConstantStatus::NonConstant) {
				issue(*argument, "Argument " + std::to_string(index + 1) + " of annotation \"@" + use.name +
					"\" isn't a constant expression.");
				valid = false; return {};
			}
			if (value.status == ConstantStatus::Value && !convertible(value, argument_spec.type)) {
				issue(*argument, "Invalid argument for annotation \"@" + use.name + "\": argument " +
					std::to_string(index + 1) + " should be \"" + expected_type(argument_spec.type) +
					"\" but is \"" + value.type + "\".");
				valid = false; return {};
			}
			values.push_back(std::move(value));
		}
		return values;
	}

	bool has_static(const SyntaxNode *node) const {
		return node && (field(*node, "static") != nullptr || trim(document.text(*node)).starts_with("static "));
	}

	const ClassRecord *owner(const Use &use) const {
		return document.class_at(use.target_node ? use.target_node->range.start : use.annotation->range.start);
	}

	bool class_inherits(const ClassRecord *record, std::string_view native_name) const {
		if (!record) return false;
		ResolvedType expected{TypeKind::NativeClass, std::string(native_name), "native:" + std::string(native_name), true};
		ResolvedType actual{TypeKind::ScriptClass, record->symbol.name, record->symbol.id, true};
		return workspace.is_assignable(expected, actual);
	}

	void validate_abstract_classes() {
		std::unordered_set<const ClassRecord *> reported;
		for (const auto &[record, function] : abstract_functions) {
			if (auto *body = field(*function, "body")) {
				analysis.issues.push_back({body->range, "An abstract function cannot have a body."});
			}
			if (!record || abstract_classes.contains(record) || !reported.insert(record).second) continue;
			analysis.issues.push_back({record->symbol.range, "Class \"" + record->symbol.name +
				"\" is not abstract but contains abstract methods. Mark the class as \"@abstract\" or remove \"@abstract\" from all methods in this class."});
		}
	}

	ResolvedType variable_type(const Use &use) const {
		if (!use.target_node) return ResolvedType::unknown();
		auto *record = owner(use);
		if (auto *type = field(*use.target_node, "type")) {
			auto spelling = trim(document.text(*type));
			if (spelling != ":=") return workspace.type_from_name(spelling, record);
		}
		if (auto *value = field(*use.target_node, "value")) {
			std::vector<std::string> stack;
			return workspace.infer_expression(std::string(document.text(*value)), document, record, value->range.start, stack);
		}
		return {TypeKind::Variant, "Variant"};
	}

	std::string string_value(const ConstantValue &value) const {
		if (auto *text = std::get_if<std::string>(&value.value)) return *text;
		return {};
	}

	void validate_export_arguments(const Use &use, const std::vector<ConstantValue> &values) {
		if (use.name == "export") return;
		for (size_t index = 0; index < values.size(); ++index) {
			if (values[index].status != ConstantStatus::Value || !std::holds_alternative<std::string>(values[index].value)) continue;
			auto text = string_value(values[index]);
			if (use.name != "export_placeholder" && text.empty()) {
				issue(*use.arguments[index], "Argument " + std::to_string(index + 1) + " of annotation \"@" + use.name + "\" is empty.");
				return;
			}
			if (use.name != "export_placeholder" && text.find(',') != std::string::npos) {
				issue(*use.arguments[index], "Argument " + std::to_string(index + 1) + " of annotation \"@" + use.name +
					"\" contains a comma. Use separate arguments instead.");
				return;
			}
			if (use.name == "export_flags") {
				auto separator = text.find(':');
				auto flag_name = text.substr(0, separator);
				if (flag_name.empty()) {
					issue(*use.arguments[index], "Invalid argument " + std::to_string(index + 1) +
						" of annotation \"@export_flags\": Expected flag name."); return;
				}
				if (separator != std::string::npos) {
					auto raw = text.substr(separator + 1);
					if (raw.empty()) { issue(*use.arguments[index], "Invalid argument " + std::to_string(index + 1) +
						" of annotation \"@export_flags\": Expected flag value."); return; }
					auto parsed = integer_literal(raw);
					if (!parsed) { issue(*use.arguments[index], "Invalid argument " + std::to_string(index + 1) +
						" of annotation \"@export_flags\": The flag value must be a valid integer."); return; }
					if (*parsed < 1 || *parsed >= (int64_t{1} << 32)) { issue(*use.arguments[index], "Invalid argument " +
						std::to_string(index + 1) + " of annotation \"@export_flags\": The flag value must be at least 1 and at most 2 ** 32 - 1."); return; }
				} else if (index >= 32) {
					issue(*use.arguments[index], "Invalid argument " + std::to_string(index + 1) +
						" of annotation \"@export_flags\": Starting from argument 33, the flag value must be specified explicitly."); return;
				}
			}
			if (use.name == "export_node_path") {
				auto *record = owner(use);
				auto type = workspace.type_from_name(text, record);
				if (!type.known()) {
					issue(*use.arguments[index], "Invalid argument " + std::to_string(index + 1) +
						" of annotation \"@export_node_path\": The class \"" + text + "\" was not found in the global scope."); return;
				}
				ResolvedType node{TypeKind::NativeClass, "Node", "native:Node", true};
				type.instance = true;
				if (!workspace.is_assignable(node, type)) {
					issue(*use.arguments[index], "Invalid argument " + std::to_string(index + 1) +
						" of annotation \"@export_node_path\": The class \"" + text + "\" does not inherit \"Node\"."); return;
				}
			}
		}
	}

	template <typename Names>
	bool type_is(const ResolvedType &type, const Names &names) const {
		if (type.kind == TypeKind::Variant || !type.known()) return true;
		for (auto name : names) if (type.name == name) return true;
		if (type.name == "Array" && !type.arguments.empty()) return type_is(type.arguments.front(), names);
		for (auto packed : {std::pair{"PackedByteArray", "int"}, {"PackedInt32Array", "int"}, {"PackedInt64Array", "int"},
				{"PackedFloat32Array", "float"}, {"PackedFloat64Array", "float"}, {"PackedStringArray", "String"},
				{"PackedColorArray", "Color"}}) {
			if (type.name == packed.first) for (auto name : names) if (name == packed.second) return true;
		}
		return false;
	}
	bool type_is(const ResolvedType &type, std::initializer_list<std::string_view> names) const {
		return type_is<std::initializer_list<std::string_view>>(type, names);
	}

	std::string export_type_error(const Use &use, const ResolvedType &type,
			const std::vector<std::string_view> &base_types) const {
		std::vector<std::string> types;
		for (auto base : base_types) {
			types.push_back(std::string(base));
			types.push_back("Array[" + std::string(base) + "]");
			if (base == "int") {
				types.insert(types.end(), {"PackedByteArray", "PackedInt32Array", "PackedInt64Array"});
			} else if (base == "float") {
				types.insert(types.end(), {"PackedFloat32Array", "PackedFloat64Array"});
			} else if (base == "String") types.push_back("PackedStringArray");
			else if (base == "Color") types.push_back("PackedColorArray");
		}
		std::string choices;
		for (size_t index = 0; index < types.size(); ++index) {
			if (index) choices += index + 1 == types.size() ? (types.size() == 2 ? " or " : ", or ") : ", ";
			choices += "\"" + types[index] + "\"";
		}
		return "\"@" + use.name + "\" annotation requires a variable of type " + choices +
			", but type \"" + type.display() + "\" was given instead.";
	}

	bool exportable_type(const ResolvedType &type) const {
		if (!type.known() || type.kind == TypeKind::Variant || type.kind == TypeKind::Enum ||
				type.kind == TypeKind::Callable || type.kind == TypeKind::Signal) return true;
		if (type.kind == TypeKind::Builtin) {
			for (const auto &argument : type.arguments) if (!exportable_type(argument)) return false;
			return true;
		}
		if (type.kind != TypeKind::NativeClass && type.kind != TypeKind::ScriptClass) return false;
		ResolvedType node{TypeKind::NativeClass, "Node", "native:Node", true};
		ResolvedType resource{TypeKind::NativeClass, "Resource", "native:Resource", true};
		return workspace.is_assignable(node, type) || workspace.is_assignable(resource, type);
	}

	bool contains_node_type(const ResolvedType &type) const {
		ResolvedType node{TypeKind::NativeClass, "Node", "native:Node", true};
		if ((type.kind == TypeKind::NativeClass || type.kind == TypeKind::ScriptClass) &&
				workspace.is_assignable(node, type)) return true;
		for (const auto &argument : type.arguments) if (contains_node_type(argument)) return true;
		return false;
	}

	void validate_export(const Use &use, const std::vector<ConstantValue> &values, AnnotationRule rule) {
		if (has_static(use.target_node)) {
			issue(*use.annotation, "Annotation \"@" + use.name + "\" cannot be applied to a static variable."); return;
		}
		auto &seen = applied[use.target_node];
		if (!seen.insert("export").second) {
			issue(*use.annotation, "Annotation \"@" + use.name + "\" cannot be used with another \"@export\" annotation."); return;
		}
		if (rule == AnnotationRule::ExportToolButton && !tool_script) {
			issue(*use.annotation, "Tool buttons can only be used in tool scripts (add \"@tool\" to the top of the script)."); return;
		}
		auto type = variable_type(use);
		if (rule == AnnotationRule::ExportToolButton) {
			if (!type_is(type, {"Callable"})) issue(*use.annotation,
				"\"@export_tool_button\" annotation requires a variable of type \"Callable\", but type \"" + type.display() + "\" was given instead.");
			return;
		}
		if (rule != AnnotationRule::Export) return;
		validate_export_arguments(use, values);
		if (use.name == "export") {
			if (!field(*use.target_node, "type") && !field(*use.target_node, "value")) {
				issue(*use.annotation, "Cannot use simple \"@export\" annotation with variable without type or initializer, since type can't be inferred."); return;
			}
			if (!type.known()) return;
			if (!exportable_type(type)) {
				issue(*use.annotation, "Export type can only be built-in, a resource, a node, or an enum."); return;
			}
			if (contains_node_type(type) && !class_inherits(owner(use), "Node")) {
				issue(*use.annotation, "Node export is only supported in Node-derived classes, but the current class inherits \"" +
					(owner(use) ? owner(use)->extends_text : std::string("Unknown")) + "\".");
			}
			return;
		}
		std::vector<std::string_view> expected;
		if (use.name == "export_enum") expected = {"int", "String"};
		else if (use.name == "export_multiline") expected = {"String", "Dictionary"};
		else if (use.name == "export_range" || use.name == "export_exp_easing") expected = {"float"};
		else if (use.name == "export_color_no_alpha") expected = {"Color"};
		else if (use.name == "export_node_path") expected = {"NodePath"};
		else if (use.name == "export_flags" || use.name.starts_with("export_flags_")) expected = {"int"};
		else if (use.name == "export_file" || use.name == "export_file_path" || use.name == "export_dir" ||
				use.name == "export_global_file" || use.name == "export_global_dir" || use.name == "export_placeholder") expected = {"String"};
		bool compatible = !expected.empty() && type_is(type, expected);
		if (!compatible && expected.size() == 1 && (expected.front() == "int" || expected.front() == "float")) {
			compatible = type_is(type, expected.front() == "int" ?
				std::initializer_list<std::string_view>{"float"} : std::initializer_list<std::string_view>{"int"});
		}
		if (!expected.empty() && !compatible) {
			issue(*use.annotation, export_type_error(use, type, expected));
		}
	}

	void validate_warning(const Use &use, const std::vector<ConstantValue> &values, bool region) {
		for (size_t index = 0; index < values.size(); ++index) {
			if (values[index].status != ConstantStatus::Value) continue;
			auto original = string_value(values[index]);
			auto name = lowercase(original);
			if (!registry.valid_warning(name)) {
				issue(*use.annotation, "Invalid warning name: \"" + original + "\"."); continue;
			}
			if (!region) {
				Range target = use.target_node ? use.target_node->range : use.annotation->range;
				analysis.suppressions.add(name, target);
			} else if (use.name == "warning_ignore_start") {
				if (auto found = warning_regions.find(name); found != warning_regions.end()) {
					issue(*use.annotation, "Warning \"" + uppercase(name) + "\" is already being ignored by \"@warning_ignore_start\" at line " +
						std::to_string(found->second.line + 1) + ".");
				} else warning_regions[name] = use.annotation->range.end;
			} else {
				auto found = warning_regions.find(name);
				if (found == warning_regions.end()) issue(*use.annotation,
					"Warning \"" + uppercase(name) + "\" is not being ignored by \"@warning_ignore_start\".");
				else {
					analysis.suppressions.add(name, {found->second, use.annotation->range.start});
					warning_regions.erase(found);
				}
			}
		}
	}

	static std::string uppercase(std::string value) {
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
			return static_cast<char>(std::toupper(character));
		});
		return value;
	}

	void validate_rpc(const Use &use, const std::vector<ConstantValue> &values) {
		if (!applied[use.target_node].insert("rpc").second) {
			issue(*use.annotation, "RPC annotations can only be used once per function."); return;
		}
		unsigned locality = 0, permission = 0, transfer = 0;
		for (size_t index = 0; index < values.size() && index < 3; ++index) {
			if (values[index].status != ConstantStatus::Value) continue;
			auto value = string_value(values[index]);
			if (value == "call_local" || value == "call_remote") ++locality;
			else if (value == "any_peer" || value == "authority") ++permission;
			else if (value == "reliable" || value == "unreliable" || value == "unreliable_ordered") ++transfer;
			else issue(*use.annotation, "Invalid RPC argument. Must be one of: \"call_local\"/\"call_remote\" (local calls), \"any_peer\"/\"authority\" (permission), \"reliable\"/\"unreliable\"/\"unreliable_ordered\" (transfer mode).");
		}
		if (locality > 1) issue(*use.annotation, "Invalid RPC config. The locality (\"call_local\"/\"call_remote\") must be specified no more than once.");
		else if (permission > 1) issue(*use.annotation, "Invalid RPC config. The permission (\"any_peer\"/\"authority\") must be specified no more than once.");
		else if (transfer > 1) issue(*use.annotation, "Invalid RPC config. The transfer mode (\"reliable\"/\"unreliable\"/\"unreliable_ordered\") must be specified no more than once.");
	}

	void validate(Use &use) {
		if (!use.name_node || use.annotation->has_error) return;
		auto *definition = registry.find(use.name);
		if (!definition) {
			if (!registry.strict_unknown_names()) return;
			if (use.name == "deprecated") issue(*use.annotation, "\"@deprecated\" annotation does not exist. Use \"## @deprecated: Reason here.\" instead.");
			else if (use.name == "experimental") issue(*use.annotation, "\"@experimental\" annotation does not exist. Use \"## @experimental: Reason here.\" instead.");
			else if (use.name == "tutorial") issue(*use.annotation, "\"@tutorial\" annotation does not exist. Use \"## @tutorial(Title): https://example.com\" instead.");
			else issue(*use.annotation, "Unrecognized annotation: \"@" + use.name + "\".");
			return;
		}
		if (!annotation_target_has(definition->targets, use.level)) {
			if (annotation_target_has(definition->targets, AnnotationTarget::Script)) {
				issue(*use.annotation, "Annotation \"@" + use.name + "\" must be at the top of the script, before \"extends\" and \"class_name\".");
			} else issue(*use.annotation, "Annotation \"@" + use.name + "\" is not allowed in this level.");
			return;
		}
		if (use.target == AnnotationTarget::None) {
			issue(*use.annotation, "Annotation \"@" + use.name + "\" does not precede a valid target, so it will have no effect."); return;
		}
		if (!annotation_target_has(definition->targets, use.target)) {
			issue(*use.annotation, "Annotation \"@" + use.name + "\" cannot be applied to a " + use.target_name + "."); return;
		}
		bool arguments_valid = false;
		auto values = validate_arguments(use, *definition, arguments_valid);
		if (!arguments_valid) return;

		switch (definition->rule) {
			case AnnotationRule::Tool:
				if (tool_script) issue(*use.annotation, "\"@tool\" annotation can only be used once.");
				else tool_script = true;
				break;
			case AnnotationRule::Icon:
				if (!applied[use.target_node].insert("icon").second) issue(*use.annotation, "\"@icon\" annotation can only be used once.");
				else if (!values.empty() && values.front().status == ConstantStatus::Value && string_value(values.front()).empty())
					issue(*use.arguments.front(), "\"@icon\" annotation argument must contain the path to the icon.");
				break;
			case AnnotationRule::StaticUnload:
				if (!applied[use.target_node].insert("static_unload").second)
					issue(*use.annotation, "\"@static_unload\" annotation can only be used once per script.");
				break;
			case AnnotationRule::Abstract:
				if (has_static(use.target_node)) issue(*use.annotation, "\"@abstract\" annotation cannot be applied to static functions.");
				else if (!applied[use.target_node].insert("abstract").second) issue(*use.annotation,
					use.target == AnnotationTarget::Function ? "\"@abstract\" annotation can only be used once per function." :
					"\"@abstract\" annotation can only be used once per class.");
				else if (use.target == AnnotationTarget::Function && use.target_node)
					abstract_functions.push_back({owner(use), use.target_node});
				else if (auto *record = owner(use)) abstract_classes.insert(record);
				break;
			case AnnotationRule::Onready:
				if (!class_inherits(owner(use), "Node")) issue(*use.annotation, "\"@onready\" can only be used in classes that inherit \"Node\".");
				else if (has_static(use.target_node)) issue(*use.annotation, "\"@onready\" annotation cannot be applied to a static variable.");
				else if (!applied[use.target_node].insert("onready").second) issue(*use.annotation, "\"@onready\" annotation can only be used once per variable.");
				break;
			case AnnotationRule::Export:
			case AnnotationRule::ExportStorage:
			case AnnotationRule::ExportCustom:
			case AnnotationRule::ExportToolButton:
				validate_export(use, values, definition->rule); break;
			case AnnotationRule::WarningIgnore: validate_warning(use, values, false); break;
			case AnnotationRule::WarningRegion: validate_warning(use, values, true); break;
			case AnnotationRule::Rpc: validate_rpc(use, values); break;
			case AnnotationRule::ExportGroup:
				if (!use.class_scope) issue(*use.annotation, "Unexpected standalone annotation.");
				break;
			case AnnotationRule::None: break;
		}
	}
};

AnnotationAnalysis AnnotationAnalyzer::run(const Workspace &workspace, const Document &document,
		const AnnotationRegistry &registry) {
	return AnnotationAnalyzerImpl(workspace, document, registry).run();
}

} // namespace gdscript_lsp
