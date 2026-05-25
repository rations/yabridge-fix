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
#include <string>

namespace fs = std::filesystem;

enum class LibArchitecture { Lib32, Lib64 };

inline std::string arch_to_string(LibArchitecture arch) {
    return arch == LibArchitecture::Lib32 ? "32-bit" : "64-bit";
}

// The x86-win / x86_64-win directory name inside a VST3 bundle
inline const char* vst_arch_string(LibArchitecture arch) {
    return arch == LibArchitecture::Lib32 ? "x86-win" : "x86_64-win";
}

struct NativeFile {
    enum class Kind { Symlink, Regular, Directory } kind;
    fs::path path;

    static NativeFile make_symlink(fs::path p) { return {Kind::Symlink, std::move(p)}; }
    static NativeFile make_regular(fs::path p) { return {Kind::Regular, std::move(p)}; }
    static NativeFile make_directory(fs::path p) { return {Kind::Directory, std::move(p)}; }
};
