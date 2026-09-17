#pragma once

#include "core/types.hpp"

namespace gdscript_lsp {

// Source-order preorder traversal. Returning false prunes a subtree. Visitors
// must not change child vectors while their nodes are on the work stack.
template <class Node, class Visitor>
void walk_syntax(Node &root, Visitor &&visit) {
	std::vector<Node *> pending{&root};
	while (!pending.empty()) {
		auto *node = pending.back();
		pending.pop_back();
		if (!visit(*node)) continue;
		for (auto child = node->children.rbegin(); child != node->children.rend(); ++child)
			pending.push_back(&*child);
	}
}

} // namespace gdscript_lsp
