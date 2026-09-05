cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

project(gdscript-lsp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${configuration} "${CMAKE_BINARY_DIR}/bin")
endforeach()

include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

# SOURCE_SUBDIR points at a deliberately absent directory so FetchContent only
# populates the pinned sources. Defining the small static targets here keeps the
# install package limited to gdscript-lsp's own runtime files.
FetchContent_Declare(
  tree_sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
  GIT_TAG v0.26.11
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _gdscript_lsp_no_subdirectory
)
FetchContent_Declare(
  tree_sitter_gdscript
  GIT_REPOSITORY https://github.com/PrestonKnopp/tree-sitter-gdscript.git
  GIT_TAG d2a0ee914d297b873a40dd4596bd1f7157ebc52b
  PATCH_COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/apply_encoded_patch.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/patches/tree-sitter-gdscript-v6.1.0.patch.gz.b64"
    "<SOURCE_DIR>"
  SOURCE_SUBDIR _gdscript_lsp_no_subdirectory
)
FetchContent_Declare(
  nlohmann_json_source
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _gdscript_lsp_no_subdirectory
)
FetchContent_MakeAvailable(tree_sitter tree_sitter_gdscript nlohmann_json_source)

add_library(tree-sitter-vendor STATIC
  "${tree_sitter_SOURCE_DIR}/lib/src/lib.c"
)
target_include_directories(tree-sitter-vendor
  PUBLIC "${tree_sitter_SOURCE_DIR}/lib/include"
  PRIVATE "${tree_sitter_SOURCE_DIR}/lib/src"
)
target_compile_definitions(tree-sitter-vendor PRIVATE
  _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE _BSD_SOURCE _DARWIN_C_SOURCE
)

add_library(tree-sitter-gdscript-vendor STATIC
  "${tree_sitter_gdscript_SOURCE_DIR}/src/parser.c"
  "${tree_sitter_gdscript_SOURCE_DIR}/src/scanner.c"
)
target_include_directories(tree-sitter-gdscript-vendor
  PRIVATE
    "${tree_sitter_SOURCE_DIR}/lib/src"
    "${tree_sitter_SOURCE_DIR}/lib/include"
    "${tree_sitter_gdscript_SOURCE_DIR}/src"
)

add_library(nlohmann-json-vendor INTERFACE)
target_include_directories(nlohmann-json-vendor INTERFACE
  "${nlohmann_json_source_SOURCE_DIR}/include"
)

file(GLOB GDSCRIPT_LSP_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
)
add_library(gdscript-lsp-core STATIC ${GDSCRIPT_LSP_CORE_SOURCES})
target_include_directories(gdscript-lsp-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(gdscript-lsp-core
  PUBLIC tree-sitter-vendor tree-sitter-gdscript-vendor nlohmann-json-vendor
)

file(GLOB GDSCRIPT_LSP_SERVER_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/lsp/*.cpp"
)
add_executable(gdscript-lsp ${GDSCRIPT_LSP_SERVER_SOURCES})
target_link_libraries(gdscript-lsp PRIVATE gdscript-lsp-core Threads::Threads)
if(WIN32)
  target_link_libraries(gdscript-lsp PRIVATE ws2_32)
endif()

if(MSVC)
  target_compile_options(gdscript-lsp-core PRIVATE /W4 /permissive-)
  target_compile_options(gdscript-lsp PRIVATE /W4 /permissive-)
else()
  target_compile_options(gdscript-lsp-core PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(gdscript-lsp PRIVATE -Wall -Wextra -Wpedantic)
endif()

set(GDSCRIPT_LSP_API_SOURCE
  "${CMAKE_CURRENT_SOURCE_DIR}/addons/gdscript_lsp/data/godot-4.6-extension-api.json"
)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/share/gdscript-lsp")
configure_file(
  "${GDSCRIPT_LSP_API_SOURCE}"
  "${CMAKE_BINARY_DIR}/share/gdscript-lsp/godot-4.6-extension-api.json"
  COPYONLY
)

include(CTest)
if(BUILD_TESTING)
  foreach(test_name core caret_context completion_provider incremental_update)
    add_executable(${test_name}-tests "tests/${test_name}_tests.cpp")
    target_link_libraries(${test_name}-tests PRIVATE gdscript-lsp-core Threads::Threads)
    if(MSVC)
      target_compile_options(${test_name}-tests PRIVATE /W4 /permissive-)
    else()
      target_compile_options(${test_name}-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME ${test_name}-tests COMMAND ${test_name}-tests)
    set_tests_properties(${test_name}-tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
  endforeach()
  add_test(
    NAME lsp-smoke
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp_smoke.py"
            "$<TARGET_FILE:gdscript-lsp>"
  )
  set_tests_properties(lsp-smoke PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  add_executable(broken-syntax-tests tests/broken_syntax_tests.cpp)
  target_link_libraries(broken-syntax-tests PRIVATE gdscript-lsp-core Threads::Threads)
  add_test(NAME semantic-recovery
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/broken_syntax.py"
            "$<TARGET_FILE:gdscript-lsp>" "$<TARGET_FILE:broken-syntax-tests>")
  set_tests_properties(semantic-recovery PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 300)
endif()

include(GNUInstallDirs)
install(TARGETS gdscript-lsp RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${GDSCRIPT_LSP_API_SOURCE}"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/gdscript-lsp"
)
install(FILES README.md THIRD_PARTY_NOTICES.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/gdscript-lsp"
)
