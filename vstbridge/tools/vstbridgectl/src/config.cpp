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

#include "config.h"

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#define TOML_EXCEPTIONS 1
#include <toml++/toml.hpp>

#include "files.h"
#include "util.h"

// ─── XDG helpers ──────────────────────────────────────────────────────────────

static fs::path xdg_config_home() {
    const char* v = std::getenv("XDG_CONFIG_HOME");
    if (v && v[0] != '\0')
        return fs::path(v);
    const char* home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("$HOME is not set");
    return fs::path(home) / ".config";
}

static fs::path xdg_data_home() {
    const char* v = std::getenv("XDG_DATA_HOME");
    if (v && v[0] != '\0')
        return fs::path(v);
    const char* home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("$HOME is not set");
    return fs::path(home) / ".local" / "share";
}

fs::path vstbridge_data_home() {
    return xdg_data_home() / "vstbridge";
}

fs::path vstbridgectl_config_dir() {
    return xdg_config_home() / "vstbridgectl";
}

fs::path vstbridge_vst2_home() {
    const char* home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("$HOME is not set");
    return fs::path(home) / ".vst" / "vstbridge";
}

fs::path vstbridge_vst3_home() {
    const char* home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("$HOME is not set");
    return fs::path(home) / ".vst3" / "vstbridge";
}

fs::path vstbridge_clap_home() {
    const char* clap_path_env = std::getenv("CLAP_PATH");
    if (clap_path_env && clap_path_env[0] != '\0') {
        std::string cp(clap_path_env);
        auto colon = cp.find(':');
        std::string first = (colon != std::string::npos) ? cp.substr(0, colon) : cp;
        return fs::path(first) / "vstbridge";
    }
    const char* home = std::getenv("HOME");
    if (!home)
        throw std::runtime_error("$HOME is not set");
    return fs::path(home) / ".clap" / "vstbridge";
}

std::optional<fs::path> which(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env)
        return std::nullopt;

    std::istringstream iss(path_env);
    std::string dir;
    while (std::getline(iss, dir, ':')) {
        fs::path candidate = fs::path(dir) / name;
        if (access(candidate.c_str(), X_OK) == 0)
            return candidate;
    }
    return std::nullopt;
}

// ─── Config read / write ──────────────────────────────────────────────────────

static fs::path config_file_path() {
    return vstbridgectl_config_dir() / "config.toml";
}

Config Config::read() {
    fs::path path = config_file_path();

    if (!fs::exists(path)) {
        Config defaults;
        defaults.write();
        return defaults;
    }

    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("Could not read config file at '" + path.string() + "'");
    std::string toml_str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    toml::table tbl;
    try {
        tbl = toml::parse(toml_str);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error("Failed to parse '" + path.string() + "': " +
                                 std::string(e.what()));
    }

    Config c;

    if (auto v = tbl["vstbridge_home"].value<std::string>())
        c.vstbridge_home = fs::path(*v);

    if (auto* arr = tbl["plugin_dirs"].as_array()) {
        for (auto& item : *arr)
            if (auto s = item.value<std::string>())
                c.plugin_dirs.insert(fs::path(*s));
    }

    if (auto v = tbl["vst2_location"].value<std::string>()) {
        if (*v == "inline")
            c.vst2_location = Vst2InstallationLocation::Inline;
        else
            c.vst2_location = Vst2InstallationLocation::Centralized;
    }

    c.no_verify = tbl["no_verify"].value_or(false);

    if (auto* arr = tbl["blacklist"].as_array()) {
        for (auto& item : *arr)
            if (auto s = item.value<std::string>())
                c.blacklist.insert(fs::path(*s));
    }

    if (auto* sub = tbl["last_known_config"].as_table()) {
        KnownConfig kc;
        kc.wine_version = sub->at("wine_version").value_or(std::string{});
        kc.vstbridge_host_hash = sub->at("vstbridge_host_hash").value_or(int64_t{0});
        c.last_known_config = kc;
    }

    return c;
}

void Config::write() const {
    fs::path dir = vstbridgectl_config_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        throw std::runtime_error("Could not create config directory '" + dir.string() +
                                 "': " + ec.message());

    toml::table tbl;

    if (vstbridge_home)
        tbl.insert_or_assign("vstbridge_home", vstbridge_home->string());

    toml::array dirs_arr;
    for (const auto& p : plugin_dirs)
        dirs_arr.push_back(p.string());
    tbl.insert_or_assign("plugin_dirs", std::move(dirs_arr));

    tbl.insert_or_assign("vst2_location",
                         vst2_location == Vst2InstallationLocation::Inline ? "inline"
                                                                            : "centralized");
    tbl.insert_or_assign("no_verify", no_verify);

    toml::array bl_arr;
    for (const auto& p : blacklist)
        bl_arr.push_back(p.string());
    tbl.insert_or_assign("blacklist", std::move(bl_arr));

    if (last_known_config) {
        toml::table sub;
        sub.insert_or_assign("wine_version", last_known_config->wine_version);
        sub.insert_or_assign("vstbridge_host_hash", last_known_config->vstbridge_host_hash);
        tbl.insert_or_assign("last_known_config", std::move(sub));
    }

    std::ostringstream oss;
    oss << tbl;

    fs::path path = config_file_path();
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Failed to write config file to '" + path.string() + "'");
    out << oss.str();
}

// ─── Config::files() ──────────────────────────────────────────────────────────

VstbridgeFiles Config::files() const {
    auto data_home = vstbridge_data_home();

    // Standard library search directories
    static const std::vector<fs::path> lib_dirs = {
        "/usr/lib",
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib64",
        "/usr/local/lib",
        "/usr/local/lib/x86_64-linux-gnu",
        "/usr/local/lib64",
    };

    fs::path vst2_chainloader;
    if (vstbridge_home) {
        auto candidate = *vstbridge_home / VST2_CHAINLOADER_NAME;
        if (!fs::exists(candidate))
            throw std::runtime_error(std::string("Could not find '") + VST2_CHAINLOADER_NAME +
                                     "' in '" + vstbridge_home->string() + "'");
        vst2_chainloader = candidate;
    } else {
        bool found = false;
        for (const auto& dir : lib_dirs) {
            auto candidate = dir / VST2_CHAINLOADER_NAME;
            if (fs::exists(candidate)) {
                vst2_chainloader = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            auto candidate = data_home / VST2_CHAINLOADER_NAME;
            if (fs::exists(candidate)) {
                vst2_chainloader = candidate;
            } else {
                throw std::runtime_error(
                    std::string("Could not find '") + VST2_CHAINLOADER_NAME +
                    "' in either '/usr/lib' or '" + data_home.string() +
                    "'. You can override the default search path using 'vstbridgectl set "
                    "--path=<path>'.");
            }
        }
    }

    LibArchitecture vst2_arch = get_elf_architecture(vst2_chainloader);

    std::optional<std::pair<fs::path, LibArchitecture>> vst3_chainloader;
    {
        auto candidate = vst2_chainloader.parent_path() / VST3_CHAINLOADER_NAME;
        if (fs::exists(candidate))
            vst3_chainloader = {candidate, get_elf_architecture(candidate)};
    }

    std::optional<std::pair<fs::path, LibArchitecture>> clap_chainloader;
    {
        auto candidate = vst2_chainloader.parent_path() / CLAP_CHAINLOADER_NAME;
        if (fs::exists(candidate))
            clap_chainloader = {candidate, get_elf_architecture(candidate)};
    }

    auto host_exe = which(VSTBRIDGE_HOST_EXE_NAME);
    std::optional<fs::path> host_exe_so;
    if (host_exe) {
        auto so = host_exe->parent_path() / (host_exe->filename().string() + ".so");
        if (fs::exists(so))
            host_exe_so = so;
    }

    auto host_32_exe = which(VSTBRIDGE_HOST_32_EXE_NAME);
    std::optional<fs::path> host_32_exe_so;
    if (host_32_exe) {
        auto so = host_32_exe->parent_path() / (host_32_exe->filename().string() + ".so");
        if (fs::exists(so))
            host_32_exe_so = so;
    }

    return VstbridgeFiles{
        vst2_chainloader,
        vst2_arch,
        vst3_chainloader,
        clap_chainloader,
        host_exe,
        host_exe_so,
        host_32_exe,
        host_32_exe_so,
    };
}

// ─── Config::search_directories() ─────────────────────────────────────────────

std::map<const fs::path*, SearchResults> Config::search_directories() const {
    std::map<const fs::path*, SearchResults> results;
    for (const auto& dir : plugin_dirs) {
        auto sr = index(dir, blacklist).search();
        results.emplace(&dir, std::move(sr));
    }
    return results;
}
