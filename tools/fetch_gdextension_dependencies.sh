#!/usr/bin/env sh
set -eu

deps_dir="${1:-.deps}"
godot_cpp_commit=26fb7ab5821e6a1096f62c22f7462d1d70caa332
scons_tag=4.8.1
mkdir -p "$deps_dir"

if [ ! -d "$deps_dir/godot-cpp" ]; then
	git init "$deps_dir/godot-cpp"
	git -C "$deps_dir/godot-cpp" remote add origin https://github.com/godotengine/godot-cpp.git
	git -C "$deps_dir/godot-cpp" fetch --depth 1 origin "$godot_cpp_commit"
	git -C "$deps_dir/godot-cpp" checkout --detach FETCH_HEAD
fi

if [ ! -d "$deps_dir/scons" ]; then
	git clone --depth 1 --branch "$scons_tag" https://github.com/SCons/scons.git "$deps_dir/scons"
fi
