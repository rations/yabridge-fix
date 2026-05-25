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

#include "blacklist.h"

#include <iostream>

namespace blacklist {

void add_path(Config& config, fs::path path) {
    config.blacklist.insert(std::move(path));
    config.write();
}

void remove_path(Config& config, const fs::path& path) {
    config.blacklist.erase(path);
    config.write();
}

void list_paths(const Config& config) {
    for (const auto& p : config.blacklist)
        std::cout << p.string() << "\n";
}

void clear(Config& config) {
    config.blacklist.clear();
    config.write();
}

}  // namespace blacklist
