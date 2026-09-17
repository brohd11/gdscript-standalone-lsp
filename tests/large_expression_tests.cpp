#include "core/document.hpp"
#include "core/syntax_walk.hpp"
#include "core/text.hpp"
#include "core/workspace.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

using namespace gdscript_lsp;
namespace {
void require(bool condition, const std::string &message) {
	if (!condition) throw std::runtime_error(message);
}

struct TemporaryProject {
	std::filesystem::path path = std::filesystem::temp_directory_path() /
		("gdscript-wide-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	TemporaryProject() {
		std::filesystem::create_directories(path);
		path = std::filesystem::canonical(path);
		std::ofstream(path / "project.godot") << "[application]\nconfig/name=\"Wide expressions\"\n";
	}
	~TemporaryProject() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

void check_tree(const Document &document, size_t terms) {
	size_t operators = 0;
	walk_syntax(document.syntax_root(), [&](const SyntaxNode &node) {
		require(node.start_byte <= node.end_byte && node.end_byte <= document.source().size(), "syntax byte range");
		if (node.kind == "binary_operator") ++operators;
		return true;
	});
	// depth_19 also has an addition.
	require(operators == terms, "all wide operators retained");
	require(document.classes().size() == 1 && document.classes().front().members.size() == 24, "all 24 methods retained");
	require(document.syntax_errors().empty(), "valid fixture parses without errors");
}

void check_diagnostics(Workspace &workspace, const std::string &uri) {
	for (const auto &diagnostic : workspace.diagnostics(uri))
		require(false, "unexpected diagnostic: " + diagnostic.code + ": " + diagnostic.message);
}

std::string expression(size_t terms, std::string_view operand = "value") {
	std::string result(operand);
	for (size_t index = 1; index < terms; ++index) result += " + " + std::string(operand);
	return result;
}

void run() {
	std::ifstream input("tests/fixtures/large_expressions/limits.gd.txt");
	const std::string fixture{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	require(!fixture.empty(), "fixture available");
	const auto start = fixture.find("\treturn value + value", fixture.find("static func wide")) + 8;
	const auto end = fixture.find('\n', start);
	require(start < end, "wide fixture expression found");
	TemporaryProject project;
	const auto path = project.path / "limits.gd";
	const auto api = std::filesystem::absolute("data/godot-4.6-extension-api.json");
	Workspace workspace;
	const auto uri = workspace.uri_for_path(path);
	int64_t version = 1;
	for (size_t terms : {2050U, 8192U}) {
		auto source = fixture;
		if (terms != 2050) source.replace(start, end - start, expression(terms));
		std::cerr << "large expressions: " << terms << " terms, parse/clone\n";
		{
			Document eager(uri, "res://limits.gd", source, version);
			check_tree(eager, terms);
			auto copy = eager.clone_for_workspace();
			check_tree(*copy, terms);
			Document deferred(uri, "res://limits.gd", source, version, Document::Analysis::Deferred);
			auto analyzed = deferred.clone_for_workspace();
			check_tree(*analyzed, terms);
			SyntaxNode assigned;
			assigned = eager.syntax_root();
			assigned = analyzed->syntax_root();
		}
		std::ofstream(path) << source;
		std::string error;
		std::cerr << "large expressions: workspace open/diagnostics/outline\n";
		require(workspace.open(project.path, api, &error), error);
		check_diagnostics(workspace, uri);
		auto operand = byte_to_position(source, start);
		require(workspace.hover(uri, operand).has_value(), "hover at the deepest operand");
		require(!workspace.definition(uri, operand).empty(), "definition at the deepest operand");
		require(!workspace.completion(uri, operand).empty(), "completion inside wide expression");
		auto outline = workspace.document_outline(uri);
		require(!outline.symbols.empty(), "outline available");
		auto wide_type = workspace.resolve_type(uri, {87, 0}, "wide(1)");
		require(wide_type.name == "int", "wide callable retains int return");
		std::cerr << "large expressions: update/diagnostics/close\n";
		Document deferred(uri, "res://limits.gd", source, version++, Document::Analysis::Deferred);
		auto updated_source = source + "\n# incremental edit 😀\n";
		Document updated(uri, "res://limits.gd", updated_source, version++, deferred, Document::Analysis::Deferred);
		require(updated.used_incremental_parse(), "incremental parse used");
		require(workspace.update_document(updated, &error), error);
		check_diagnostics(workspace, uri);
		// Readiness must survive another edit after the large one.
		require(workspace.update_document(uri, "extends RefCounted\nfunc later() -> int:\n\treturn 1\n", version++, &error), error);
		check_diagnostics(workspace, uri);
		require(workspace.close_document(uri, &error), error);
		check_diagnostics(workspace, uri); // Restores the wide disk source.
	}

	for (bool at_start : {true, false}) {
		auto sum = expression(2050);
		if (at_start) sum.replace(0, 5, "missing_left");
		else sum.replace(sum.size() - 5, 5, "missing_right");
		auto source = "extends RefCounted\nfunc wide(value: int):\n\treturn " + sum + "\n";
		std::string error;
		require(workspace.update_document(uri, source, version++, &error), error);
		auto diagnostics = workspace.diagnostics(uri);
		const auto spelling = at_start ? "missing_left" : "missing_right";
		require(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic &issue) {
			return issue.message.find(spelling) != std::string::npos && issue.range.start.line == 2 &&
				issue.range.start.character == (at_start ? 8U : 8U + sum.size() - std::string(spelling).size());
		}), "invalid operand diagnosed at the correct end of the expression");
	}

	{
		std::string nested;
		for (size_t index = 1; index < 2050; ++index) nested += "value + (";
		nested += "value";
		nested.append(2049, ')');
		std::string error;
		require(workspace.update_document(uri, "extends RefCounted\nfunc wide(value: int) -> int:\n\treturn " + nested + "\n", version++, &error), error);
		check_diagnostics(workspace, uri);
	}
	{
		// A preload path exposes the reducer's numeric result. Both precedence
		// and left associativity matter: 2050 - 2 * 1023 == 4.
		auto arithmetic = expression(2050, "1") + " - 2 * 1023";
		auto source = "extends RefCounted\nfunc reduced():\n\treturn preload(\"%d.gd\" % [(" + arithmetic + ")])\n";
		std::string error;
		require(workspace.update_document(uri, source, version++, &error), error);
		auto issues = workspace.diagnostics(uri);
		require(issues.size() == 1 && issues.front().code == "missing-preload" &&
			issues.front().message.find("res://4.gd") != std::string::npos, "constant reduction preserves precedence and value");
	}

	// Small and large versions exercise precedence, parentheses, casts, type
	// tests and the grammar's attribute suffix repair through the same evaluator.
	for (const auto &tail : {" - (2 * 3)", " as int", " is int", " + Vector2.ONE.x"}) {
		std::vector<std::string> expected;
		for (size_t terms : {2U, 2050U}) {
			auto source = "extends RefCounted\nfunc wide(value: int):\n\treturn " + expression(terms) + tail + "\n";
			std::string error;
			require(workspace.update_document(uri, source, version++, &error), error);
			std::vector<std::string> actual;
			for (const auto &issue : workspace.diagnostics(uri)) actual.push_back(issue.code + ":" + issue.message);
			if (terms == 2) expected = actual;
			else require(actual == expected, "large mixed expression matches ordinary diagnostics");
		}
	}
}

std::exception_ptr failure;
#ifdef _WIN32
DWORD WINAPI worker(void *) {
#else
void *worker(void *) {
#endif
	try { run(); } catch (...) { failure = std::current_exception(); }
	return 0;
}
}

int main() {
#ifdef _WIN32
	auto thread = CreateThread(nullptr, 512 * 1024, worker, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	if (!thread) return 1;
	WaitForSingleObject(thread, INFINITE);
	CloseHandle(thread);
#else
	pthread_attr_t attributes;
	if (pthread_attr_init(&attributes)) return 1;
	if (pthread_attr_setstacksize(&attributes, 512 * 1024)) return 1;
	pthread_t thread;
	const auto created = pthread_create(&thread, &attributes, worker, nullptr);
	pthread_attr_destroy(&attributes);
	if (created || pthread_join(thread, nullptr)) return 1;
#endif
	if (failure) {
		try { std::rethrow_exception(failure); }
		catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
		return 1;
	}
	std::cout << "large expression tests passed\n";
}
