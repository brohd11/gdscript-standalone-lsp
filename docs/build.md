# Building from source

On Unix, the shortest build path is:

```sh
make deps
make
build/gdscript-lsp
```

A pinned CMake build is available on Linux, macOS, and Windows:

```sh
cmake -S . -B build-cmake
cmake --build build-cmake --config Release
ctest --test-dir build-cmake -C Release --output-on-failure
```

On Windows, run these commands from a Visual Studio developer shell or let
CMake select an installed Visual Studio generator. To create a self-contained
layout with the executable and API metadata, run:

```sh
cmake --install build-cmake --config Release --prefix dist
```

The Make and CMake install targets include both the executable and its bundled
API metadata. Semantic GDScript addon tests remain a manual editor-console
check. See [development](development.md) for the automated test and benchmark
commands.
