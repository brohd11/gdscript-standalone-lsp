#!/usr/bin/env python3

import os

godot_cpp = ARGUMENTS.get("godot_cpp", ".deps/godot-cpp")
env = SConscript(os.path.join(godot_cpp, "SConstruct"), {"api_version": "4.6"})
env.Append(
    CPPPATH=[
        "src",
        ".deps/tree-sitter/lib/include",
        ".deps/tree-sitter/lib/src",
        ".deps/tree-sitter-gdscript/src",
        ".deps/json/include",
    ],
    CXXFLAGS=["-std=c++20"],
    CPPDEFINES=["_DEFAULT_SOURCE"],
)

sources = (
    Glob("src/core/*.cpp")
    + Glob("src/gdextension/*.cpp")
    + [
        ".deps/tree-sitter/lib/src/lib.c",
        ".deps/tree-sitter-gdscript/src/parser.c",
        ".deps/tree-sitter-gdscript/src/scanner.c",
    ]
)

library = env.SharedLibrary(
    "addons/gdscript_lsp/bin/libgdscript_lsp{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
    source=sources,
)
env.NoCache(library)
Default(library)
