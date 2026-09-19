#include "core/source_lexical.hpp"
#include "core/document.hpp"
#include "core/caret_context.hpp"
#include "core/syntax_checks.hpp"
#include "core/text.hpp"
#include "core/workspace.hpp"

#include <filesystem>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>
#include <utility>

using namespace gdscript_lsp;

namespace {
int failures = 0;
void expect(bool condition, const std::string &message) {
	if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
}

int main() {
	using Kind = SourceLexicalKind;
	for (const std::string quote : {"\"", "'", "\"\"\"", "'''"}) {
		for (const std::string prefix : {"", "r", "&", "^"}) {
			auto literal = prefix + quote + "hello # is: ([{\nfunc fake(): pass\n" + quote;
			auto source = "var value = " + literal + " # comment '\"\\\r\nvar after = 1\n";
			SourceLexicalMap map(source);
			expect(map.spans().size() == 2, "one literal and one comment: " + prefix + quote);
			if (map.spans().size() != 2) continue;
			auto &span = map.spans().front();
			expect(span.closed && span.content_begin == 12 + prefix.size() + quote.size() &&
				span.end == 12 + literal.size(), "literal boundaries: " + prefix + quote);
			auto kind = prefix == "&" ? Kind::StringName : prefix == "^" ? Kind::NodePath : Kind::String;
			expect(span.kind == kind && span.raw == (prefix == "r"), "literal prefix classification");
			expect(map.context_at(span.quote_begin) == Kind::Code, "caret before opening quote");
			expect(map.context_at(span.quote_begin + 1) == kind, "caret after opening quote");
			expect(map.context_at(span.content_end) == kind, "caret before closing quote");
			expect(map.context_at(span.end) == Kind::Code, "caret after closing delimiter");
			expect(map.kind_at(source.find("fake")) == kind, "multiline content stays in literal");
			expect(map.context_at(source.find("# comment")) == Kind::Code, "caret before comment");
			expect(map.context_at(source.find("# comment") + 1) == Kind::Comment, "caret inside comment");
			expect(map.context_at(source.find("var after")) == Kind::Code, "CRLF terminates comment");
			auto masked = map.masked_text(source, 0, source.size());
			expect(masked.size() == source.size() && masked.find("fake") == std::string::npos &&
				masked.find("var after") == source.find("var after"), "mask preserves offsets and hides literal content");
			for (size_t i = 0; i < source.size(); ++i) if (source[i] == '\n' || source[i] == '\r')
				expect(masked[i] == source[i], "mask preserves newline bytes");
		}
	}
	for (const std::string quote : {"\"", "'", "\"\"\"", "'''"}) {
		for (const std::string prefix : {"", "r"}) {
			auto source = prefix + quote + "escaped \\" + quote + " still string\n" + quote + " + after";
			SourceLexicalMap map(source);
			expect(map.spans().size() == 1 && map.spans()[0].closed &&
				map.kind_at(source.find("still")) == Kind::String && map.code()[source.find("after")],
				"escaped delimiter: " + prefix + quote);
		}
	}
	for (const std::string source : {"'unfinished\nfunc fake():", "r\"\"\"unfinished\\", "# comment"}) {
		SourceLexicalMap map(source);
		expect(map.context_at(source.size()) != Kind::Code, "unfinished region extends to EOF");
	}
	{
		std::string source = "var 位置 = \"🌍 # is:\"; var after = 1\n";
		SourceLexicalMap map(source);
		expect(map.masked_text(source, 0, source.size()).find("after") == source.find("after"), "UTF-8 byte offsets unchanged");
		expect(byte_to_position(source, source.find("after")).character < source.find("after"), "positions use original UTF-16 conversion");
	}
	// Exercise insertion boundaries after an escaped quote, not rfind(quote).
	{
		std::string source = "func f():\n\tself.call(\"ab\\\"cd";
		Document document("file:///test.gd", "res://test.gd", source);
		auto caret = analyze_caret(document, byte_to_position(source, source.size()));
		expect(caret.call && caret.call->in_string && caret.call->string_prefix == "ab\\\"cd",
			"member-string completion keeps prefix across escaped quotes");
	}
	// Lexical state must be independent of tree-sitter recovery and document
	// analysis mode. Compare every caret boundary after edits to delimiters.
	std::vector<std::string> edits = {
		"func f():\n\tvar text = \"\"\"first\nfunc fake(): pass\n\"\"\"\n\treturn text\n",
		"func f():\n\tvar text = \"first\nfunc fake(): pass\n\"\n\treturn text\n",
		"func f():\n\tvar text = \"first\nfunc fake(): pass\n",
		"func f():\n\tvar text = \"first\\\" # still literal\n\"\n\treturn text\n",
		"func f():\n\tvar text = 1 # \"first\n\treturn text\n",
		"func f():\n\tvar text = \"first\"\n\treturn text\n"
	};
	auto previous = std::make_shared<Document>("file:///test.gd", "res://test.gd", edits.front());
	for (const auto &source : edits) {
		auto deferred = std::make_shared<Document>("file:///test.gd", "res://test.gd", source, 2, *previous, Document::Analysis::Deferred);
		auto clone = deferred->clone_for_workspace();
		Document fresh("file:///test.gd", "res://test.gd", source);
		expect(&deferred->lexical() == &clone->lexical(), "workspace clone shares immutable lexical map");
		expect(clone->lexical().code() == fresh.lexical().code(), "incremental and fresh classifications agree");
		for (size_t i = 0; i <= source.size(); ++i)
			expect(clone->lexical().context_at(i) == fresh.lexical().context_at(i), "incremental caret context agrees");
		for (const auto &record : clone->classes()) for (const auto &member : record.members)
			expect(member.name != "fake", "recovery does not invent functions inside strings");
		expect(lexical_issues(*clone).size() == lexical_issues(fresh).size(), "incremental lexical diagnostics agree");
		previous = std::move(clone);
	}
	{
		Workspace workspace;
		auto fixture = std::filesystem::weakly_canonical("tests/fixtures/basic");
		std::string error;
		expect(workspace.open(fixture, fixture / "extension_api.json", &error), "path test workspace opens: " + error);
		auto source_uri = workspace.uri_for_path(fixture / "lexical_source.gd");
		auto target_uri = workspace.uri_for_path(fixture / "child.gd");
		int version = 0;
		for (auto [source, dependency] : std::vector<std::pair<std::string, bool>>{
			{"# \"res://child.gd\"\n", false},
			{"var text = \"\"\"example 'res://child.gd'\nfunc fake(): pass\n\"\"\"\n", false},
			{"var text = \"\"\"res://child.gd\"\"\"\n", true},
			{"var text = r\"res://child.gd\"\n", true},
			{"var text = \"\"\"res://child.gd", false},
			{"var text = \"res://child.gd\"\n", true},
		}) {
			expect(workspace.update_document(source_uri, source, ++version, &error), "path overlay updates: " + error);
			auto affected = workspace.affected_documents({target_uri});
			expect((std::find(affected.begin(), affected.end(), source_uri) != affected.end()) == dependency,
				"only complete path literals create dependencies: " + source);
		}
		const std::string source = "class Product:\n\tvar x: float\nvar items: Dictionary[String, Product]\n"
			"func make(_text: String) -> Product:\n\treturn Product.new()\n";
		expect(workspace.update_document(source_uri, source, ++version, &error), "expression overlay updates");
		for (const std::string literal : {"\"a\\\"b ) ] #\"", "\"\"\"a\n)b].#\"\"\"", "r\"a\\\"b )\""}) {
			auto type = workspace.resolve_type(source_uri, {4, 1}, "make(" + literal + ").x");
			expect(type.name == "float", "terminal call and member scanning ignore literal punctuation: " + literal);
			auto indexed = workspace.resolve_type(source_uri, {4, 1}, "items[" + literal + "].x");
			expect(indexed.name == "float", "subscript scanning ignores literal punctuation: " + literal);
		}
	}
	if (failures) return 1;
	std::cout << "source lexical tests passed\n";
}
