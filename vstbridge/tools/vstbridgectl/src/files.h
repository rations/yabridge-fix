// vstbridge: a Wine VST/CLAP plugin bridge
// Copyright (C) 2020-2024 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "config.h"
#include "types.h"

namespace fs = std::filesystem;

// ─── Plugin types ──────────────────────────────────────────────────────────────

struct Vst2Plugin {
    fs::path path;
    LibArchitecture architecture;
    std::optional<fs::path> subdirectory;

    fs::path centralized_native_target() const;
    fs::path centralized_windows_target() const;
    fs::path inline_native_target() const;
};

enum class Vst3ModuleKind { Legacy, Bundle };

struct Vst3Module {
    Vst3ModuleKind kind;
    fs::path module_path;  // bundle root for Bundle, .vst3 file for Legacy
    LibArchitecture architecture;
    std::optional<fs::path> subdirectory;

    fs::path original_path() const { return module_path; }
    std::string original_module_name() const;
    fs::path original_module_path() const;
    std::optional<fs::path> original_resources_dir() const;
    std::optional<fs::path> original_moduleinfo_path() const;

    fs::path target_bundle_home() const;
    fs::path target_native_module_path(const VstbridgeFiles* files) const;
    fs::path target_windows_module_path() const;
    fs::path target_resources_dir() const;
    fs::path target_moduleinfo_path() const;

    const char* type_str() const {
        return kind == Vst3ModuleKind::Bundle ? "bundle" : "legacy";
    }
};

struct ClapPlugin {
    fs::path path;
    LibArchitecture architecture;
    std::optional<fs::path> subdirectory;

    fs::path native_target() const;
    fs::path windows_target() const;
};

using Plugin = std::variant<Vst2Plugin, Vst3Module, ClapPlugin>;

// ─── Search results ────────────────────────────────────────────────────────────

struct SearchResults {
    std::vector<Plugin> plugins;
    std::vector<fs::path> skipped_files;
    std::vector<NativeFile> so_files;

    // Returns map: original plugin path → (plugin ref, current NativeFile status)
    std::map<fs::path, std::pair<const Plugin*, std::optional<NativeFile>>>
    installation_status(const Config& config, const VstbridgeFiles* files) const;

    // .so files in the search dir that don't have a corresponding .dll (inline mode)
    std::vector<const NativeFile*> vst2_inline_orphans(const Config& config) const;
};

// ─── Index / search ────────────────────────────────────────────────────────────

struct SearchIndex {
    std::vector<std::pair<fs::path, std::optional<fs::path>>> dll_files;
    std::vector<std::pair<fs::path, std::optional<fs::path>>> vst3_files;
    std::vector<std::pair<fs::path, std::optional<fs::path>>> clap_files;
    std::vector<NativeFile> so_files;

    SearchResults search() const;
};

SearchIndex index(const fs::path& directory, const std::set<fs::path>& blacklist);
