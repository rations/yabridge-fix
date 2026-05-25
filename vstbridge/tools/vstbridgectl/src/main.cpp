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

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include <CLI/CLI.hpp>

#include "actions.h"
#include "actions/blacklist.h"
#include "config.h"
#include "util.h"

namespace fs = std::filesystem;

// ─── Path validators ───────────────────────────────────────────────────────────

// Verify a path exists (file or directory)
static std::string validate_path_exists(const std::string& s) {
    fs::path p(s);
    if (!fs::exists(p))
        return "File or directory could not be found.";
    return {};
}

// Verify a path is an existing directory
static std::string validate_directory(const std::string& s) {
    fs::path p(s);
    if (!fs::exists(p))
        return "Directory could not be found.";
    if (!fs::is_directory(p))
        return "Path is not a directory.";
    return {};
}

// Validate that a path is in a known set, trying various normalisations.
// Returns the canonical path from the set, or throws on failure.
static fs::path validate_path_from_set(const std::string& s,
                                       const std::set<fs::path>& candidates) {
    fs::path p(s);
    if (!p.is_absolute()) {
        std::error_code ec;
        p = fs::current_path(ec) / p;
    }

    if (candidates.count(p))
        return p;

    auto norm = normalize_path(p);
    // Strip trailing slash
    std::string norm_str = norm.string();
    while (norm_str.size() > 1 && norm_str.back() == '/')
        norm_str.pop_back();
    fs::path norm_no_slash(norm_str);

    if (candidates.count(norm_no_slash))
        return norm_no_slash;

    // With trailing slash variant
    fs::path norm_with_slash = norm / "";
    if (candidates.count(norm_with_slash))
        return norm_with_slash;

    std::string options;
    for (const auto& c : candidates)
        options += "\n  " + c.string();
    throw std::runtime_error("Not a known path.\n\n       Possible options are:" + options);
}

int main(int argc, char** argv) {
    // Append ~/.local/share/vstbridge to PATH so vstbridge-host.exe can be found
    try {
        auto data_home = vstbridge_data_home();
        std::string path_env = data_home.string();
        if (const char* existing = std::getenv("PATH"))
            path_env = std::string(existing) + ":" + path_env;
        setenv("PATH", path_env.c_str(), 1);
    } catch (...) {
    }

    Config config;
    try {
        config = Config::read();
    } catch (const std::exception& e) {
        std::cerr << "Error reading config: " << e.what() << "\n";
        return 1;
    }

    // Snapshot sets for rm validators (captured before CLI parsing)
    std::set<fs::path> plugin_dirs_set = config.plugin_dirs;
    std::set<fs::path> blacklist_set = config.blacklist;

    // ─── CLI setup ──────────────────────────────────────────────────────────

    CLI::App app{"vstbridgectl - Manage vstbridge plugin installations", "vstbridgectl"};
    app.require_subcommand(1);

    // ── add ──────────────────────────────────────────────────────────────────
    auto* add_cmd = app.add_subcommand("add", "Add a plugin install location");
    std::string add_path_str;
    add_cmd->add_option("path", add_path_str,
                        "Path to a directory containing Windows VST2, VST3, or CLAP plugins")
        ->required()
        ->check([](const std::string& s) { return validate_directory(s); });

    // ── rm ───────────────────────────────────────────────────────────────────
    auto* rm_cmd = app.add_subcommand("rm", "Remove a plugin install location");
    std::string rm_path_str;
    rm_cmd->add_option("path", rm_path_str, "Path to a previously added directory")
        ->required()
        ->check([&plugin_dirs_set](const std::string& s) -> std::string {
            try {
                validate_path_from_set(s, plugin_dirs_set);
                return {};
            } catch (const std::exception& e) {
                return e.what();
            }
        });

    // ── list ─────────────────────────────────────────────────────────────────
    app.add_subcommand("list", "List the plugin install locations");

    // ── status ───────────────────────────────────────────────────────────────
    app.add_subcommand("status", "Show the installation status for all plugins");

    // ── sync ─────────────────────────────────────────────────────────────────
    auto* sync_cmd = app.add_subcommand("sync", "Set up or update vstbridge for all plugins");
    SyncOptions sync_opts;
    sync_cmd->add_flag("-f,--force", sync_opts.force, "Always update files, even if not necessary");
    sync_cmd->add_flag("-n,--no-verify", sync_opts.no_verify, "Skip post-installation setup checks");
    sync_cmd->add_flag("-p,--prune", sync_opts.prune, "Remove unrelated or leftover .so files");
    sync_cmd->add_flag("-v,--verbose", sync_opts.verbose,
                       "Print information about plugins being set up or skipped");

    // ── set ──────────────────────────────────────────────────────────────────
    auto* set_cmd = app.add_subcommand("set", "Change the vstbridge path (advanced)");
    set_cmd->require_option(1);
    SetOptions set_opts;
    std::string set_path_str;
    bool set_path_auto = false;
    std::string vst2_location_str;
    std::string no_verify_str;

    set_cmd->add_option("--path", set_path_str,
                        "Path to the directory containing 'libvstbridge-chainloader-{clap,vst2,vst3}.so'")
        ->check([](const std::string& s) { return validate_directory(s); });
    set_cmd->add_flag("--path-auto", set_path_auto, "Automatically locate vstbridge's files");
    set_cmd->add_option("--vst2-location", vst2_location_str, "Where to set up VST2 plugins")
        ->check(CLI::IsMember({"centralized", "inline"}));
    set_cmd->add_option("--no-verify", no_verify_str, "Always skip post-installation setup checks")
        ->check(CLI::IsMember({"true", "false"}));

    // ── blacklist ────────────────────────────────────────────────────────────
    auto* bl_cmd = app.add_subcommand("blacklist", "Manage the indexing blacklist (advanced)");
    bl_cmd->require_subcommand(1);

    auto* bl_add_cmd = bl_cmd->add_subcommand("add", "Add a path to the blacklist");
    std::string bl_add_path_str;
    bl_add_cmd->add_option("path", bl_add_path_str, "Path to a file or a directory")
        ->required()
        ->check([](const std::string& s) { return validate_path_exists(s); });

    auto* bl_rm_cmd = bl_cmd->add_subcommand("rm", "Remove a path from the blacklist");
    std::string bl_rm_path_str;
    bl_rm_cmd->add_option("path", bl_rm_path_str, "Path to a previously added file or directory")
        ->required()
        ->check([&blacklist_set](const std::string& s) -> std::string {
            try {
                validate_path_from_set(s, blacklist_set);
                return {};
            } catch (const std::exception& e) {
                return e.what();
            }
        });

    bl_cmd->add_subcommand("list", "List the blacklisted paths");
    bl_cmd->add_subcommand("clear", "Clear the entire blacklist");

    // ─── Parse ──────────────────────────────────────────────────────────────

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    // ─── Dispatch ───────────────────────────────────────────────────────────

    try {
        if (app.got_subcommand("add")) {
            std::error_code ec;
            auto path = fs::canonical(fs::path(add_path_str), ec);
            if (ec)
                path = fs::path(add_path_str);
            add_directory(config, path);

        } else if (app.got_subcommand("rm")) {
            auto path = validate_path_from_set(rm_path_str, plugin_dirs_set);
            remove_directory(config, path);

        } else if (app.got_subcommand("list")) {
            list_directories(config);

        } else if (app.got_subcommand("status")) {
            show_status(config);

        } else if (app.got_subcommand("sync")) {
            do_sync(config, sync_opts);

        } else if (app.got_subcommand("set")) {
            if (!set_path_str.empty()) {
                std::error_code ec;
                auto path = fs::canonical(fs::path(set_path_str), ec);
                if (ec)
                    path = fs::path(set_path_str);
                set_opts.path = path;
            }
            set_opts.path_auto = set_path_auto;
            if (!vst2_location_str.empty())
                set_opts.vst2_location = (vst2_location_str == "inline") ? Vst2Location::Inline
                                                                          : Vst2Location::Centralized;
            if (!no_verify_str.empty())
                set_opts.no_verify = (no_verify_str == "true");
            set_settings(config, set_opts);

        } else if (app.got_subcommand("blacklist")) {
            if (bl_cmd->got_subcommand("add")) {
                std::error_code ec;
                auto path = fs::canonical(fs::path(bl_add_path_str), ec);
                if (ec)
                    path = fs::path(bl_add_path_str);
                blacklist::add_path(config, path);
            } else if (bl_cmd->got_subcommand("rm")) {
                auto path = validate_path_from_set(bl_rm_path_str, blacklist_set);
                blacklist::remove_path(config, path);
            } else if (bl_cmd->got_subcommand("list")) {
                blacklist::list_paths(config);
            } else if (bl_cmd->got_subcommand("clear")) {
                blacklist::clear(config);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
