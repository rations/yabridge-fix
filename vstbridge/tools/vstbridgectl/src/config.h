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

#include "types.h"

namespace fs = std::filesystem;

// Forward-declare SearchResults to avoid a circular include with files.h
struct SearchResults;

static constexpr const char* CLAP_CHAINLOADER_NAME = "libvstbridge-chainloader-clap.so";
static constexpr const char* VST2_CHAINLOADER_NAME = "libvstbridge-chainloader-vst2.so";
static constexpr const char* VST3_CHAINLOADER_NAME = "libvstbridge-chainloader-vst3.so";
static constexpr const char* VSTBRIDGE_HOST_EXE_NAME = "vstbridge-host.exe";
static constexpr const char* VSTBRIDGE_HOST_32_EXE_NAME = "vstbridge-host-32.exe";

enum class Vst2InstallationLocation { Centralized, Inline };

struct KnownConfig {
    std::string wine_version;
    int64_t vstbridge_host_hash{};

    bool operator==(const KnownConfig& o) const {
        return wine_version == o.wine_version && vstbridge_host_hash == o.vstbridge_host_hash;
    }
};

struct VstbridgeFiles {
    fs::path vst2_chainloader;
    LibArchitecture vst2_chainloader_arch;

    // nullopt if vstbridge was compiled without VST3 support
    std::optional<std::pair<fs::path, LibArchitecture>> vst3_chainloader;
    // nullopt if vstbridge was compiled without CLAP support
    std::optional<std::pair<fs::path, LibArchitecture>> clap_chainloader;

    std::optional<fs::path> vstbridge_host_exe;
    std::optional<fs::path> vstbridge_host_exe_so;
    std::optional<fs::path> vstbridge_host_32_exe;
    std::optional<fs::path> vstbridge_host_32_exe_so;
};

struct Config {
    // Path to the directory containing libvstbridge-chainloader-*.so
    std::optional<fs::path> vstbridge_home;
    // Directories to search for Windows plugins (kept sorted, no duplicates)
    std::set<fs::path> plugin_dirs;
    Vst2InstallationLocation vst2_location = Vst2InstallationLocation::Centralized;
    bool no_verify = false;
    std::set<fs::path> blacklist;
    std::optional<KnownConfig> last_known_config;

    static Config read();
    void write() const;

    VstbridgeFiles files() const;

    // Returns a map from plugin directory path → SearchResults for each dir
    std::map<const fs::path*, SearchResults> search_directories() const;
};

// XDG helpers
fs::path vstbridge_data_home();  // ~/.local/share/vstbridge (or $XDG_DATA_HOME/vstbridge)
fs::path vstbridgectl_config_dir();  // $XDG_CONFIG_HOME/vstbridgectl

fs::path vstbridge_vst2_home();
fs::path vstbridge_vst3_home();
fs::path vstbridge_clap_home();

// Search PATH for an executable; returns its full path or nullopt
std::optional<fs::path> which(const std::string& name);
