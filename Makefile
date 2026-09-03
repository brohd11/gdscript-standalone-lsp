CXX ?= c++
CC ?= cc
AR ?= ar
DEPS_DIR ?= .deps
BUILD_DIR ?= build
SCONS ?= python3 $(DEPS_DIR)/scons/scripts/scons.py
GODOT ?= godot
PREFIX ?= /usr/local

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -pthread -MMD -MP -Isrc \
	-I$(DEPS_DIR)/tree-sitter/lib/include -I$(DEPS_DIR)/json/include
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -fPIC -I$(DEPS_DIR)/tree-sitter/lib/src \
	-I$(DEPS_DIR)/tree-sitter/lib/include -I$(DEPS_DIR)/tree-sitter-gdscript/src
LDFLAGS += -pthread

CORE_CPP := $(wildcard src/core/*.cpp)
CORE_OBJ := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(CORE_CPP))
TS_OBJ := $(BUILD_DIR)/vendor/tree_sitter.o $(BUILD_DIR)/vendor/gdscript_parser.o $(BUILD_DIR)/vendor/gdscript_scanner.o
DEPFILES := $(CORE_OBJ:.o=.d) $(BUILD_DIR)/lsp/main.d $(BUILD_DIR)/tests/core_tests.d \
	$(BUILD_DIR)/tests/caret_context_tests.d $(BUILD_DIR)/tools/dump_tree.d

-include $(DEPFILES)

.PHONY: all deps test test-conformance gdextension test-gdextension install clean dump-tree
all: $(BUILD_DIR)/gdscript-lsp

deps:
	@tools/fetch_dependencies.sh "$(DEPS_DIR)"

$(BUILD_DIR)/gdscript-lsp: $(CORE_OBJ) $(TS_OBJ) $(BUILD_DIR)/lsp/main.o
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/core-tests: $(CORE_OBJ) $(TS_OBJ) $(BUILD_DIR)/tests/core_tests.o
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD_DIR)/caret-context-tests: $(CORE_OBJ) $(TS_OBJ) $(BUILD_DIR)/tests/caret_context_tests.o
	$(CXX) $^ $(LDFLAGS) -o $@

test: $(BUILD_DIR)/core-tests $(BUILD_DIR)/caret-context-tests $(BUILD_DIR)/gdscript-lsp
	$(BUILD_DIR)/core-tests
	$(BUILD_DIR)/caret-context-tests
	python3 tests/lsp_smoke.py $(BUILD_DIR)/gdscript-lsp

test-conformance: $(BUILD_DIR)/gdscript-lsp
	python3 tools/godot_diagnostic_oracle.py $(BUILD_DIR)/gdscript-lsp "$(GODOT)"

gdextension: deps
	@tools/fetch_gdextension_dependencies.sh "$(DEPS_DIR)"
	$(SCONS) platform=linux target=template_debug

test-gdextension: gdextension
	@mkdir -p /tmp/gdscript-lsp-xdg
	XDG_DATA_HOME=/tmp/gdscript-lsp-xdg $(GODOT) --headless --path . --script res://tests/gdextension_smoke.gd

install: $(BUILD_DIR)/gdscript-lsp
	install -Dm755 $(BUILD_DIR)/gdscript-lsp "$(DESTDIR)$(PREFIX)/bin/gdscript-lsp"
	install -Dm644 addons/gdscript_lsp/data/godot-4.6-extension-api.json \
		"$(DESTDIR)$(PREFIX)/share/gdscript-lsp/godot-4.6-extension-api.json"

$(BUILD_DIR)/dump-tree: $(TS_OBJ) $(BUILD_DIR)/tools/dump_tree.o
	$(CXX) $^ $(LDFLAGS) -o $@

dump-tree: $(BUILD_DIR)/dump-tree
	@test -n "$(FILE)" || (echo "usage: make dump-tree FILE=path/to/file.gd" && exit 2)
	$(BUILD_DIR)/dump-tree "$(FILE)"

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/tests/%.o: tests/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/vendor/tree_sitter.o: $(DEPS_DIR)/tree-sitter/lib/src/lib.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vendor/gdscript_parser.o: $(DEPS_DIR)/tree-sitter-gdscript/src/parser.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/vendor/gdscript_scanner.o: $(DEPS_DIR)/tree-sitter-gdscript/src/scanner.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf "$(BUILD_DIR)"
