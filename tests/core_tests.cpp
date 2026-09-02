#include "core/text.hpp"
#include "core/uri.hpp"
#include "core/workspace.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

using namespace gdscript_lsp;

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << '\n';
		++failures;
	}
}

bool has_item(const std::vector<CompletionItem> &items, const std::string &name) {
	return std::any_of(items.begin(), items.end(), [&](const auto &item) { return item.label == name; });
}

bool has_diagnostic(const std::vector<Diagnostic> &items, const std::string &code) {
	return std::any_of(items.begin(), items.end(), [&](const auto &item) { return item.code == code; });
}

size_t diagnostic_count(const std::vector<Diagnostic> &items, const std::string &code) {
	return static_cast<size_t>(std::count_if(items.begin(), items.end(),
		[&](const auto &item) { return item.code == code; }));
}

} // namespace

int main() {
	auto fixture = std::filesystem::weakly_canonical("tests/fixtures/basic");
	Workspace workspace;
	std::string error;
	expect(workspace.open(fixture, fixture / "extension_api.json", &error), "workspace opens: " + error);
	expect(workspace.stats().document_count == 6, "all fixture scripts indexed");
	expect(workspace.native_api().version() == "4.6.3", "native API version loaded");

	auto consumer_uri = workspace.uri_for_path(fixture / "consumer.gd");
	auto completion = workspace.completion(consumer_uri, {6, 7});
	expect(has_item(completion, "own"), "completion includes direct script member");
	expect(has_item(completion, "count"), "completion includes inherited script member");
	expect(has_item(completion, "label"), "completion includes inherited method");
	expect(has_item(completion, "reference_method"), "completion includes transitive native member");

	auto local_type = workspace.resolve_type(consumer_uri, {6, 4}, "local");
	expect(local_type.kind == TypeKind::ScriptClass && local_type.name == "ChildThing", "typed local resolves to script class");
	auto child_type = workspace.resolve_type(consumer_uri, {2, 6}, "child");
	expect(child_type.kind == TypeKind::ScriptClass && child_type.name == "ChildThing", "constructor inference resolves script instance");
	auto autoload_type = workspace.resolve_type(consumer_uri, {6, 4}, "FixtureGlobal");
	expect(autoload_type.kind == TypeKind::ScriptClass && autoload_type.instance, "autoload resolves as script instance");

	auto definitions = workspace.definition(consumer_uri, {2, 15});
	expect(definitions.size() == 1 && definitions.front().uri.ends_with("/child.gd"), "global class definition resolves");
	auto symbols = workspace.document_symbols(consumer_uri);
	expect(!symbols.empty() && symbols.front().children.size() == 2, "document symbols include members");

	auto unicode = std::string("a😀b\nvalue");
	auto byte = position_to_byte(unicode, {0, 3});
	expect(byte == 5, "UTF-16 position converts surrogate pair");
	expect(byte_to_position(unicode, byte) == Position{0, 3}, "byte converts back to UTF-16 position");
	auto encoded_path = std::filesystem::path("/tmp/gdscript lsp/project.godot");
	auto encoded_uri = file_uri_for_path(encoded_path);
	expect(encoded_uri.find("gdscript%20lsp") != std::string::npos, "file URI percent-encodes spaces");
	expect(path_for_file_uri(encoded_uri) == std::filesystem::absolute(encoded_path).lexically_normal(),
		"file URI round trips to a path");
	expect(path_for_file_uri("file://localhost/tmp/project.godot") == std::filesystem::path("/tmp/project.godot"),
		"localhost file URI is accepted");
	expect(!path_for_file_uri("file://remote-host/tmp/project.godot"), "remote file URI authority is rejected");
	expect(!path_for_file_uri("file:///tmp/bad%2"), "malformed percent escape is rejected");

	auto changed = std::string(
		"class_name ChildThing extends BaseThing\n\n"
		"var own := \"child\"\n"
		"var added: int = 2\n\n"
		"func make_base() -> BaseThing:\n\treturn self\n");
	auto child_uri = workspace.uri_for_path(fixture / "child.gd");
	expect(workspace.update_document(child_uri, changed, 2, &error), "unsaved document update accepted");
	completion = workspace.completion(consumer_uri, {6, 7});
	expect(has_item(completion, "added"), "dependent completion reflects unsaved base update");
	expect(workspace.close_document(child_uri, &error), "closing overlay restores disk");
	completion = workspace.completion(consumer_uri, {6, 7});
	expect(!has_item(completion, "added"), "closing overlay invalidates dependent result");

	auto outside_uri = workspace.uri_for_path(fixture.parent_path() / "basic-sibling/escape.gd");
	expect(!workspace.update_document(outside_uri, "extends Node\n", 1, &error),
		"document overlays cannot escape the workspace by path prefix");
	Workspace invalid_api_workspace;
	expect(!invalid_api_workspace.open(fixture, fixture / "missing-extension-api.json", &error),
		"an explicitly requested missing native API is reported");

	auto diagnostic_fixture = std::filesystem::weakly_canonical("tests/fixtures/diagnostics");
	Workspace diagnostic_workspace;
	expect(diagnostic_workspace.open(diagnostic_fixture, fixture / "extension_api.json", &error),
		"diagnostic workspace opens: " + error);
	auto diagnostic_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "errors.gd");
	auto diagnostics = diagnostic_workspace.diagnostics(diagnostic_uri);
	if (diagnostic_count(diagnostics, "type-mismatch") != 2) {
		for (const auto &item : diagnostics) std::cerr << "diagnostic: " << item.code << ": " << item.message << '\n';
	}
	expect(has_diagnostic(diagnostics, "duplicate-symbol"), "duplicate member is diagnosed");
	expect(diagnostic_count(diagnostics, "unknown-type") == 2, "unknown member and parameter types are diagnosed");
	expect(diagnostic_count(diagnostics, "type-mismatch") == 2, "member and local assignment mismatches are diagnosed");
	auto duplicate = std::find_if(diagnostics.begin(), diagnostics.end(),
		[](const auto &item) { return item.code == "duplicate-symbol"; });
	expect(duplicate != diagnostics.end() && duplicate->related_information.size() == 1,
		"duplicate diagnostic points to the first declaration");

	auto unresolved_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "unresolved.gd");
	expect(has_diagnostic(diagnostic_workspace.diagnostics(unresolved_uri), "unresolved-base"),
		"unresolved base class is diagnosed");
	for (const auto &name : {"cycle_a.gd", "cycle_b.gd"}) {
		auto uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / name);
		expect(has_diagnostic(diagnostic_workspace.diagnostics(uri), "inheritance-cycle"),
			std::string("inheritance cycle is diagnosed in ") + name);
	}
	for (const auto &name : {"duplicate_a.gd", "duplicate_b.gd"}) {
		auto uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / name);
		expect(has_diagnostic(diagnostic_workspace.diagnostics(uri), "duplicate-global-class"),
			std::string("duplicate global class is diagnosed in ") + name);
	}
	auto syntax_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "syntax_error.gd");
	expect(has_diagnostic(diagnostic_workspace.diagnostics(syntax_uri), "syntax-error"), "syntax error is diagnosed");
	auto valid_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "valid_child.gd");
	expect(diagnostic_workspace.diagnostics(valid_uri).empty(),
		"assigning a derived script instance to its base type is valid");

	const std::string fixed = "extends RefCounted\n\nvar value: int = 1\n";
	expect(diagnostic_workspace.update_document(diagnostic_uri, fixed, 7, &error), "diagnostic overlay accepted");
	expect(diagnostic_workspace.diagnostics(diagnostic_uri).empty(), "fixed overlay clears diagnostics");
	expect(diagnostic_workspace.document_version(diagnostic_uri) == 7, "diagnostics expose overlay version");
	expect(diagnostic_workspace.close_document(diagnostic_uri, &error), "diagnostic overlay closes");
	expect(has_diagnostic(diagnostic_workspace.diagnostics(diagnostic_uri), "duplicate-symbol"),
		"closing overlay restores disk diagnostics");

	auto semantic_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "semantic_errors.gd");
	auto semantic_diagnostics = diagnostic_workspace.diagnostics(semantic_uri);
	const std::unordered_map<std::string, size_t> expected_semantic = {
		{"undefined-identifier", 2}, {"undefined-function", 1}, {"unknown-member", 1},
		{"not-callable", 1}, {"argument-count", 2}, {"argument-type", 2},
		{"missing-return-path", 1}, {"return-value-in-void", 2},
		{"missing-return-value", 1}, {"return-type-mismatch", 3},
	};
	for (const auto &[code, count] : expected_semantic) {
		expect(diagnostic_count(semantic_diagnostics, code) == count,
			"semantic diagnostic count for " + code + " is " + std::to_string(count));
	}
	if (semantic_diagnostics.size() != 16) {
		for (const auto &item : semantic_diagnostics) std::cerr << "semantic diagnostic: " << item.code << ": " << item.message << '\n';
	}
	expect(semantic_diagnostics.size() == 16, "semantic fixture has no unexpected diagnostics");
	auto semantic_valid_uri = diagnostic_workspace.uri_for_path(diagnostic_fixture / "semantic_valid.gd");
	auto semantic_valid_diagnostics = diagnostic_workspace.diagnostics(semantic_valid_uri);
	if (!semantic_valid_diagnostics.empty()) {
		for (const auto &item : semantic_valid_diagnostics) std::cerr << "valid semantic diagnostic: " << item.code << ": " << item.message << '\n';
	}
	expect(semantic_valid_diagnostics.empty(),
		"dynamic receivers, utilities, singletons, derived arguments, and complete returns are valid");

	Workspace legacy_api_workspace;
	auto legacy_api = std::filesystem::weakly_canonical("tests/fixtures/legacy_extension_api.json");
	expect(legacy_api_workspace.open(diagnostic_fixture, legacy_api, &error), "legacy reduced API workspace opens: " + error);
	auto legacy_call_uri = legacy_api_workspace.uri_for_path(diagnostic_fixture / "legacy_api_call.gd");
	expect(legacy_api_workspace.diagnostics(legacy_call_uri).empty(),
		"missing legacy default/vararg metadata does not create native arity errors");

	Workspace repository_workspace;
	auto repository_api = std::filesystem::weakly_canonical("addons/gdscript_lsp/data/godot-4.6-extension-api.json");
	expect(repository_workspace.open(".", repository_api, &error), "repository workspace opens: " + error);
	auto u_file_uri = repository_workspace.uri_for_path(
		std::filesystem::weakly_canonical("addons/addon_lib/brohd/alib_runtime/utils/u_file.gd"));
	auto u_file_diagnostics = repository_workspace.diagnostics(u_file_uri);
	expect(u_file_diagnostics.size() == 1 && u_file_diagnostics.front().code == "return-type-mismatch" &&
		u_file_diagnostics.front().range.start.line == 428,
		"u_file.gd retains only its genuine line 429 return mismatch");

	if (failures == 0) std::cout << "core tests passed\n";
	return failures == 0 ? 0 : 1;
}
