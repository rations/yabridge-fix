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
#include <optional>
#include <string>

#include "config.h"

namespace fs = std::filesystem;

enum class Vst2Location { Centralized, Inline };

struct SetOptions {
    std::optional<fs::path> path;
    bool path_auto = false;
    std::optional<Vst2Location> vst2_location;
    std::optional<bool> no_verify;
};

struct SyncOptions {
    bool force = false;
    bool no_verify = false;
    bool prune = false;
    bool verbose = false;
};

void add_directory(Config& config, fs::path path);
void remove_directory(Config& config, const fs::path& path);
void list_directories(const Config& config);
void show_status(const Config& config);
void set_settings(Config& config, const SetOptions& opts);
void do_sync(Config& config, const SyncOptions& opts);
