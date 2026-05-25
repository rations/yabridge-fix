# Makefile for yabridgectl
# Dependencies are fetched automatically with 'make deps' or on first build.
# Requirements: g++ (C++20), git

CXX      ?= g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra $(CXXFLAGS)
LDFLAGS  := -ldl -lpthread $(LDFLAGS)

BUILD_DIR := build
DEP_DIR   := deps

CLI11_URL        := https://github.com/CLIUtils/CLI11.git
CLI11_TAG        := v2.4.2
NLOHMANN_URL     := https://github.com/nlohmann/json.git
NLOHMANN_TAG     := v3.11.3
TOMLPP_URL       := https://github.com/marzer/tomlplusplus.git
TOMLPP_TAG       := v3.4.0

INCLUDES := \
	-Isrc \
	-I$(DEP_DIR)/cli11/include \
	-I$(DEP_DIR)/nlohmann/single_include \
	-I$(DEP_DIR)/tomlplusplus/include

SRCS := \
	src/main.cpp \
	src/config.cpp \
	src/files.cpp \
	src/util.cpp \
	src/symbols.cpp \
	src/vst3_moduleinfo.cpp \
	src/actions.cpp \
	src/actions/blacklist.cpp

OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

TARGET := $(BUILD_DIR)/vstbridgectl

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null)
GTK_LIBS   := $(shell pkg-config --libs   gtk+-3.0 2>/dev/null)

GUI_OBJS := \
	$(BUILD_DIR)/gui_main.o \
	$(BUILD_DIR)/gui/app_window.o \
	$(BUILD_DIR)/config.o \
	$(BUILD_DIR)/files.o \
	$(BUILD_DIR)/util.o \
	$(BUILD_DIR)/symbols.o \
	$(BUILD_DIR)/vst3_moduleinfo.o \
	$(BUILD_DIR)/actions.o \
	$(BUILD_DIR)/actions/blacklist.o

GUI_TARGET := $(BUILD_DIR)/vstbridgectl-gtk

.PHONY: all deps clean install

all: deps $(TARGET) $(GUI_TARGET)

# ─── Dependency fetch ─────────────────────────────────────────────────────────

$(DEP_DIR)/cli11/.git:
	git clone --depth 1 --branch $(CLI11_TAG) $(CLI11_URL) $(DEP_DIR)/cli11

$(DEP_DIR)/nlohmann/.git:
	git clone --depth 1 --branch $(NLOHMANN_TAG) $(NLOHMANN_URL) $(DEP_DIR)/nlohmann

$(DEP_DIR)/tomlplusplus/.git:
	git clone --depth 1 --branch $(TOMLPP_TAG) $(TOMLPP_URL) $(DEP_DIR)/tomlplusplus

deps: $(DEP_DIR)/cli11/.git $(DEP_DIR)/nlohmann/.git $(DEP_DIR)/tomlplusplus/.git

# ─── Build rules ──────────────────────────────────────────────────────────────

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR) $(BUILD_DIR)/actions
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/actions/%.o: src/actions/%.cpp | $(BUILD_DIR)/actions deps
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/actions:
	mkdir -p $@

$(BUILD_DIR)/gui:
	mkdir -p $@

# ─── GUI-specific build rules (need GTK_CFLAGS) ───────────────────────────────

$(BUILD_DIR)/gui_main.o: src/gui_main.cpp | $(BUILD_DIR) deps
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/gui/app_window.o: src/gui/app_window.cpp | $(BUILD_DIR)/gui deps
	$(CXX) $(CXXFLAGS) $(GTK_CFLAGS) $(INCLUDES) -c $< -o $@

$(GUI_TARGET): $(GUI_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(GTK_LIBS)

# ─── Install / clean ──────────────────────────────────────────────────────────

install: $(TARGET) $(GUI_TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/vstbridgectl
	install -Dm755 $(GUI_TARGET) $(DESTDIR)$(PREFIX)/bin/vstbridgectl-gtk

clean:
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf $(DEP_DIR)
