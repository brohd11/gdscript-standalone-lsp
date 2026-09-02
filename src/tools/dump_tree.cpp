#include <tree_sitter/api.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

extern "C" const TSLanguage *tree_sitter_gdscript(void);

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	std::ifstream stream(argv[1], std::ios::binary);
	std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	TSParser *parser = ts_parser_new();
	ts_parser_set_language(parser, tree_sitter_gdscript());
	TSTree *tree = ts_parser_parse_string(parser, nullptr, source.data(), static_cast<uint32_t>(source.size()));
	char *sexp = ts_node_string(ts_tree_root_node(tree));
	std::cout << sexp << '\n';
	free(sexp);
	ts_tree_delete(tree);
	ts_parser_delete(parser);
}
