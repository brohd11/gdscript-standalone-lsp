#!/usr/bin/env sh
set -eu

deps_dir="${1:-.deps}"
mkdir -p "$deps_dir"

clone_tag() {
	name="$1"
	tag="$2"
	url="$3"
	expected_commit="${4:-}"
	if [ -d "$deps_dir/$name" ]; then
		exit_code=0
	else
		git clone --depth 1 --branch "$tag" "$url" "$deps_dir/$name" || exit_code=$?
		if [ "${exit_code:-0}" -ne 0 ]; then
			exit "$exit_code"
		fi
	fi
	if [ -n "$expected_commit" ]; then
		actual_commit="$(git -C "$deps_dir/$name" rev-parse HEAD)"
		if [ "$actual_commit" != "$expected_commit" ]; then
			echo "$name $tag resolved to $actual_commit, expected $expected_commit" >&2
			exit 1
		fi
	fi
}

clone_tag tree-sitter v0.26.11 https://github.com/tree-sitter/tree-sitter.git
clone_tag tree-sitter-gdscript v6.1.0 https://github.com/PrestonKnopp/tree-sitter-gdscript.git \
	d2a0ee914d297b873a40dd4596bd1f7157ebc52b
clone_tag json v3.12.0 https://github.com/nlohmann/json.git

script_dir="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
grammar_patch="$script_dir/patches/tree-sitter-gdscript-v6.1.0.patch.gz.b64"
decode_grammar_patch() {
	base64 -d "$grammar_patch" | gzip -dc
}

if decode_grammar_patch | git -C "$deps_dir/tree-sitter-gdscript" apply --reverse --check - >/dev/null 2>&1; then
	:
elif decode_grammar_patch | git -C "$deps_dir/tree-sitter-gdscript" apply --check - >/dev/null; then
	decode_grammar_patch | git -C "$deps_dir/tree-sitter-gdscript" apply -
else
	echo "tree-sitter-gdscript compatibility patch does not apply cleanly" >&2
	exit 1
fi
