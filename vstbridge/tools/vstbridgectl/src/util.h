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

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

namespace fs = std::filesystem;

struct Config;

LibArchitecture get_elf_architecture(const fs::path& path);
std::optional<NativeFile> get_file_type(const fs::path& path);
LibArchitecture get_default_wine_prefix_arch();
int64_t hash_file(const fs::path& path);
fs::path normalize_path(const fs::path& path);

void copy_or_reflink(const fs::path& from, const fs::path& to);
void create_dir_all(const fs::path& path);
void remove_dir_all(const fs::path& path);
void remove_file_or_dir(const fs::path& path);
void make_symlink(const fs::path& target, const fs::path& link);
void write_file(const fs::path& path, const std::string& contents);
std::string read_to_string(const fs::path& path);
std::vector<uint8_t> read_bytes(const fs::path& path);

bool verify_path_setup();
void verify_wine_setup(Config& config);
void verify_external_dependencies();

std::string wrap_text(const std::string& text);

namespace color {
bool enabled();
std::string red(const std::string& s);
std::string green(const std::string& s);
std::string yellow(const std::string& s);
std::string cyan(const std::string& s);
std::string magenta(const std::string& s);
std::string bright_white(const std::string& s);
}  // namespace color
