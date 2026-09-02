#!/usr/bin/env sh
set -eu

deps_dir="${1:-.deps}"
mkdir -p "$deps_dir"

clone_tag() {
	name="$1"
	tag="$2"
	url="$3"
	if [ -d "$deps_dir/$name" ]; then
		exit_code=0
	else
		git clone --depth 1 --branch "$tag" "$url" "$deps_dir/$name" || exit_code=$?
		if [ "${exit_code:-0}" -ne 0 ]; then
			exit "$exit_code"
		fi
	fi
}

clone_tag tree-sitter v0.26.11 https://github.com/tree-sitter/tree-sitter.git
clone_tag tree-sitter-gdscript v6.1.0 https://github.com/PrestonKnopp/tree-sitter-gdscript.git
clone_tag json v3.12.0 https://github.com/nlohmann/json.git
