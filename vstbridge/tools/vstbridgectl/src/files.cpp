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

#include "files.h"

#include <future>
#include <iostream>
#include <unordered_map>

#include "symbols.h"
#include "util.h"

// ─── Vst2Plugin methods ────────────────────────────────────────────────────────

fs::path Vst2Plugin::centralized_native_target() const {
    auto file_name = path.filename().replace_extension(".so");
    if (subdirectory)
        return vstbridge_vst2_home() / *subdirectory / file_name;
    return vstbridge_vst2_home() / file_name;
}

fs::path Vst2Plugin::centralized_windows_target() const {
    auto file_name = path.filename();
    if (subdirectory)
        return vstbridge_vst2_home() / *subdirectory / file_name;
    return vstbridge_vst2_home() / file_name;
}

fs::path Vst2Plugin::inline_native_target() const {
    return path.parent_path() / path.filename().replace_extension(".so");
}

// ─── Vst3Module methods ────────────────────────────────────────────────────────

std::string Vst3Module::original_module_name() const {
    return module_path.filename().string();
}

fs::path Vst3Module::original_module_path() const {
    if (kind == Vst3ModuleKind::Legacy)
        return module_path;
    auto p = module_path / "Contents" / vst_arch_string(architecture) / original_module_name();
    return p;
}

std::optional<fs::path> Vst3Module::original_resources_dir() const {
    if (kind == Vst3ModuleKind::Legacy)
        return std::nullopt;
    auto p = module_path / "Contents" / "Resources";
    if (fs::exists(p))
        return p;
    return std::nullopt;
}

std::optional<fs::path> Vst3Module::original_moduleinfo_path() const {
    if (kind == Vst3ModuleKind::Legacy)
        return std::nullopt;
    auto p = module_path / "Contents" / "moduleinfo.json";
    if (fs::exists(p))
        return p;
    return std::nullopt;
}

fs::path Vst3Module::target_bundle_home() const {
    auto name = original_module_name();
    if (subdirectory)
        return vstbridge_vst3_home() / *subdirectory / name;
    return vstbridge_vst3_home() / name;
}

fs::path Vst3Module::target_native_module_path(const VstbridgeFiles* files) const {
    auto name = fs::path(original_module_name()).replace_extension(".so");
    auto base = target_bundle_home() / "Contents";
    if (files && files->vst3_chainloader) {
        if (files->vst3_chainloader->second == LibArchitecture::Lib32)
            return base / "i386-linux" / name;
    }
    return base / "x86_64-linux" / name;
}

fs::path Vst3Module::target_windows_module_path() const {
    return target_bundle_home() / "Contents" / vst_arch_string(architecture) /
           original_module_name();
}

fs::path Vst3Module::target_resources_dir() const {
    return target_bundle_home() / "Contents" / "Resources";
}

fs::path Vst3Module::target_moduleinfo_path() const {
    return target_bundle_home() / "Contents" / "moduleinfo.json";
}

// ─── ClapPlugin methods ────────────────────────────────────────────────────────

fs::path ClapPlugin::native_target() const {
    auto file_name = path.filename().replace_extension(".clap");
    if (subdirectory)
        return vstbridge_clap_home() / *subdirectory / file_name;
    return vstbridge_clap_home() / file_name;
}

fs::path ClapPlugin::windows_target() const {
    auto file_name = path.filename().replace_extension(".clap-win");
    if (subdirectory)
        return vstbridge_clap_home() / *subdirectory / file_name;
    return vstbridge_clap_home() / file_name;
}

// ─── SearchResults methods ─────────────────────────────────────────────────────

std::map<fs::path, std::pair<const Plugin*, std::optional<NativeFile>>>
SearchResults::installation_status(const Config& config, const VstbridgeFiles* files) const {
    std::unordered_map<std::string, const NativeFile*> so_map;
    for (const auto& f : so_files)
        so_map[f.path.string()] = &f;

    std::map<fs::path, std::pair<const Plugin*, std::optional<NativeFile>>> result;

    for (const auto& plugin : plugins) {
        std::visit(
            [&](const auto& p) {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, Vst2Plugin>) {
                    std::optional<NativeFile> status;
                    if (config.vst2_location == Vst2InstallationLocation::Centralized) {
                        status = get_file_type(p.centralized_native_target());
                    } else {
                        auto key = p.inline_native_target().string();
                        auto it = so_map.find(key);
                        if (it != so_map.end())
                            status = *it->second;
                    }
                    result[p.path] = {&plugin, status};
                } else if constexpr (std::is_same_v<T, Vst3Module>) {
                    auto status = get_file_type(p.target_native_module_path(files));
                    result[p.original_path()] = {&plugin, status};
                } else if constexpr (std::is_same_v<T, ClapPlugin>) {
                    auto status = get_file_type(p.native_target());
                    result[p.path] = {&plugin, status};
                }
            },
            plugin);
    }
    return result;
}

std::vector<const NativeFile*> SearchResults::vst2_inline_orphans(const Config& config) const {
    std::unordered_map<std::string, const NativeFile*> orphans;
    for (const auto& f : so_files)
        orphans[f.path.string()] = &f;

    if (config.vst2_location == Vst2InstallationLocation::Inline) {
        for (const auto& plugin : plugins) {
            if (auto* v = std::get_if<Vst2Plugin>(&plugin))
                orphans.erase(v->inline_native_target().string());
        }
    }
    // In centralized mode, any .so in a plugin directory is an orphan

    std::vector<const NativeFile*> result;
    result.reserve(orphans.size());
    for (const auto& [k, v] : orphans)
        result.push_back(v);
    return result;
}

// ─── Directory walking ─────────────────────────────────────────────────────────

// Recursively walk a directory, respecting the blacklist.
// Returns SearchIndex with all relevant files.
SearchIndex index(const fs::path& directory, const std::set<fs::path>& blacklist) {
    SearchIndex idx;

    // Pre-canonicalize blacklist entries for fast comparison
    std::set<fs::path> canonical_blacklist;
    for (const auto& b : blacklist) {
        std::error_code ec;
        auto c = fs::canonical(b, ec);
        if (!ec)
            canonical_blacklist.insert(c);
        else
            canonical_blacklist.insert(b);
    }

    size_t file_count = 0;

    // Stack-based recursive walk with symlink following
    std::function<void(const fs::path&)> walk = [&](const fs::path& dir) {
        std::error_code ec;
        fs::directory_iterator it(dir, ec);
        if (ec)
            return;

        for (const auto& entry : it) {
            auto entry_path = entry.path();

            // Blacklist check on canonical path
            {
                std::error_code cec;
                auto canon = fs::canonical(entry_path, cec);
                if (!cec && canonical_blacklist.count(canon))
                    continue;
            }

            auto symlink_st = entry.symlink_status(ec);
            if (ec) {
                // Broken symlink or permission error — include as a candidate
                // (so it can appear in so_files for pruning)
                auto ext = entry_path.extension().string();
                if (ext == ".so") {
                    idx.so_files.push_back(NativeFile::make_symlink(entry_path));
                }
                continue;
            }

            bool is_dir = fs::is_directory(symlink_st);
            bool is_sym = fs::is_symlink(symlink_st);

            // If it's a symlink to a directory, follow it
            if (is_sym) {
                std::error_code target_ec;
                auto target_st = fs::status(entry_path, target_ec);
                if (!target_ec && fs::is_directory(target_st))
                    is_dir = true;
            }

            if (is_dir) {
                walk(entry_path);
                continue;
            }

            // It's a file (or symlink to a file)
            ++file_count;
            if (file_count == 100'000) {
                std::cerr << "Indexed over 100.000 files, press Ctrl+C to cancel this operation "
                             "if this was not intentional.\n";
            }

            auto ext = entry_path.extension().string();
            // Compute subdirectory relative to search root
            auto parent = entry_path.parent_path();
            std::optional<fs::path> subdir;
            std::error_code sec;
            auto rel = fs::relative(parent, directory, sec);
            if (!sec && !rel.empty() && rel.string() != ".")
                subdir = rel;

            if (ext == ".dll") {
                idx.dll_files.emplace_back(entry_path, subdir);
            } else if (ext == ".vst3") {
                idx.vst3_files.emplace_back(entry_path, subdir);
            } else if (ext == ".clap") {
                idx.clap_files.emplace_back(entry_path, subdir);
            } else if (ext == ".so") {
                if (is_sym)
                    idx.so_files.push_back(NativeFile::make_symlink(entry_path));
                else
                    idx.so_files.push_back(NativeFile::make_regular(entry_path));
            }
        }
    };

    walk(directory);
    return idx;
}

// ─── SearchIndex::search() ─────────────────────────────────────────────────────

SearchResults SearchIndex::search() const {
    SearchResults sr;
    sr.so_files = so_files;

    // Helper: parse a PE32 binary, printing a warning and returning nullopt on error
    auto try_parse = [](const fs::path& p) -> std::optional<Pe32Info> {
        try {
            return parse_pe32_binary(p);
        } catch (const std::exception& e) {
            std::cerr << "WARNING: Skipping file during scan: " << e.what() << "\n\n";
            return std::nullopt;
        }
    };

    // Helper: check if exports contain any of the given entry points
    auto has_export = [](const std::vector<std::string>& exports,
                         std::initializer_list<const char*> names) -> bool {
        for (const char* name : names)
            for (const auto& e : exports)
                if (e == name)
                    return true;
        return false;
    };

    // Validate DLL files in parallel using std::async
    std::vector<std::future<std::optional<std::variant<Vst2Plugin, fs::path>>>> dll_futures;
    dll_futures.reserve(dll_files.size());
    for (const auto& [path, subdir] : dll_files) {
        dll_futures.push_back(std::async(std::launch::async, [path, subdir, &try_parse, &has_export] {
            auto info = try_parse(path);
            if (!info)
                return std::optional<std::variant<Vst2Plugin, fs::path>>{};
            auto arch = info->is_64_bit ? LibArchitecture::Lib64 : LibArchitecture::Lib32;
            if (has_export(info->exports, {"VSTPluginMain", "main"}))
                return std::optional<std::variant<Vst2Plugin, fs::path>>{
                    Vst2Plugin{path, arch, subdir}};
            return std::optional<std::variant<Vst2Plugin, fs::path>>{path};
        }));
    }

    for (auto& fut : dll_futures) {
        auto result = fut.get();
        if (!result)
            continue;
        if (auto* plugin = std::get_if<Vst2Plugin>(&*result))
            sr.plugins.push_back(std::move(*plugin));
        else
            sr.skipped_files.push_back(std::get<fs::path>(*result));
    }

    // Validate VST3 files in parallel
    std::vector<std::future<std::optional<std::variant<Vst3Module, fs::path>>>> vst3_futures;
    vst3_futures.reserve(vst3_files.size());
    for (const auto& [module_path, subdir] : vst3_files) {
        vst3_futures.push_back(
            std::async(std::launch::async, [module_path, subdir, &try_parse, &has_export] {
                auto info = try_parse(module_path);
                if (!info)
                    return std::optional<std::variant<Vst3Module, fs::path>>{};
                auto arch = info->is_64_bit ? LibArchitecture::Lib64 : LibArchitecture::Lib32;
                if (!has_export(info->exports, {"GetPluginFactory"}))
                    return std::optional<std::variant<Vst3Module, fs::path>>{module_path};

                // Detect if this .vst3 file is inside a bundle
                // A bundle has structure: <name>.vst3/Contents/<arch>/<name>.vst3
                auto module_name = module_path.filename();
                auto arch_dir = module_path.parent_path();
                auto contents_dir = arch_dir.parent_path();
                auto bundle_root = contents_dir.parent_path();

                bool in_bundle = false;
                if (bundle_root.filename().extension() == ".vst3") {
                    // Reconstruct expected path
                    auto reconstructed = bundle_root.parent_path() / module_name / "Contents" /
                                        vst_arch_string(arch) / module_name;
                    std::error_code ec;
                    in_bundle = fs::exists(reconstructed, ec) && !ec;
                }

                std::optional<fs::path> effective_subdir = subdir;
                if (in_bundle) {
                    // Adjust subdirectory: strip x86_64-win / Contents / bundle_name from subdir
                    if (effective_subdir) {
                        auto p = *effective_subdir;
                        // p = bundlename.vst3/Contents/x86_64-win  (relative to plugin dir)
                        if (p.has_parent_path())
                            p = p.parent_path();  // bundlename.vst3/Contents
                        if (p.has_parent_path())
                            p = p.parent_path();  // bundlename.vst3
                        if (p.has_parent_path())
                            p = p.parent_path();  // subdir before bundle
                        effective_subdir = (p.empty() || p.string() == ".") ? std::nullopt
                                                                             : std::make_optional(p);
                    }
                    return std::optional<std::variant<Vst3Module, fs::path>>{
                        Vst3Module{Vst3ModuleKind::Bundle, bundle_root, arch, effective_subdir}};
                }
                return std::optional<std::variant<Vst3Module, fs::path>>{
                    Vst3Module{Vst3ModuleKind::Legacy, module_path, arch, subdir}};
            }));
    }

    for (auto& fut : vst3_futures) {
        auto result = fut.get();
        if (!result)
            continue;
        if (auto* module = std::get_if<Vst3Module>(&*result))
            sr.plugins.push_back(std::move(*module));
        else
            sr.skipped_files.push_back(std::get<fs::path>(*result));
    }

    // Validate CLAP files in parallel
    std::vector<std::future<std::optional<std::variant<ClapPlugin, fs::path>>>> clap_futures;
    clap_futures.reserve(clap_files.size());
    for (const auto& [path, subdir] : clap_files) {
        clap_futures.push_back(
            std::async(std::launch::async, [path, subdir, &try_parse, &has_export] {
                auto info = try_parse(path);
                if (!info)
                    return std::optional<std::variant<ClapPlugin, fs::path>>{};
                auto arch = info->is_64_bit ? LibArchitecture::Lib64 : LibArchitecture::Lib32;
                if (has_export(info->exports, {"clap_entry"}))
                    return std::optional<std::variant<ClapPlugin, fs::path>>{
                        ClapPlugin{path, arch, subdir}};
                return std::optional<std::variant<ClapPlugin, fs::path>>{path};
            }));
    }

    for (auto& fut : clap_futures) {
        auto result = fut.get();
        if (!result)
            continue;
        if (auto* plugin = std::get_if<ClapPlugin>(&*result))
            sr.plugins.push_back(std::move(*plugin));
        else
            sr.skipped_files.push_back(std::get<fs::path>(*result));
    }

    return sr;
}
