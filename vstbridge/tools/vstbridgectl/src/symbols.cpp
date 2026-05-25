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

#include "symbols.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

// ─── Minimal PE32(+) parser ───────────────────────────────────────────────────
// References:
//   https://learn.microsoft.com/en-us/windows/win32/debug/pe-format

namespace {

struct DOSHeader {
    uint16_t magic;  // MZ = 0x5A4D
    uint8_t  pad[58];
    uint32_t pe_offset;  // @ 0x3C
};

struct COFFHeader {
    uint16_t machine;
    uint16_t num_sections;
    uint32_t timestamp;
    uint32_t symbol_table_ptr;
    uint32_t num_symbols;
    uint16_t opt_header_size;
    uint16_t characteristics;
};

struct SectionHeader {
    char     name[8];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t raw_size;
    uint32_t raw_offset;
    uint32_t reloc_ptr;
    uint32_t line_ptr;
    uint16_t num_relocs;
    uint16_t num_lines;
    uint32_t characteristics;
};

struct ExportDirectory {
    uint32_t characteristics;
    uint32_t timestamp;
    uint16_t major_version;
    uint16_t minor_version;
    uint32_t name_rva;
    uint32_t ordinal_base;
    uint32_t num_functions;
    uint32_t num_names;
    uint32_t addr_of_functions;   // RVA
    uint32_t addr_of_names;       // RVA to array of RVAs to name strings
    uint32_t addr_of_name_ordinals;
};

// Convert a relative virtual address to a file offset using section headers
uint32_t rva_to_offset(uint32_t rva,
                       const std::vector<SectionHeader>& sections) {
    for (const auto& sec : sections) {
        if (rva >= sec.virtual_address &&
            rva < sec.virtual_address + std::max(sec.virtual_size, sec.raw_size)) {
            return sec.raw_offset + (rva - sec.virtual_address);
        }
    }
    throw std::runtime_error("RVA out of any section");
}

template <typename T>
T read_at(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + sizeof(T) > data.size())
        throw std::runtime_error("PE read past end of file");
    T val;
    std::memcpy(&val, data.data() + offset, sizeof(T));
    return val;
}

Pe32Info parse_pe32_native(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open '" + path.string() + "'");

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());

    // DOS header
    if (data.size() < sizeof(DOSHeader))
        throw std::runtime_error("File too small for DOS header");
    uint16_t dos_magic = read_at<uint16_t>(data, 0);
    if (dos_magic != 0x5A4D)  // "MZ"
        throw std::runtime_error("Not a PE file (missing MZ header)");
    uint32_t pe_off = read_at<uint32_t>(data, 0x3C);

    // PE signature
    if (pe_off + 4 > data.size())
        throw std::runtime_error("PE offset out of bounds");
    uint32_t pe_sig = read_at<uint32_t>(data, pe_off);
    if (pe_sig != 0x00004550)  // "PE\0\0"
        throw std::runtime_error("Missing PE signature");

    size_t coff_off = pe_off + 4;
    COFFHeader coff = read_at<COFFHeader>(data, coff_off);

    // 0x014C = IMAGE_FILE_MACHINE_I386 (32-bit x86)
    // 0x8664 = IMAGE_FILE_MACHINE_AMD64 (64-bit)
    bool is_64 = (coff.machine == 0x8664);

    // Optional header starts right after COFF header
    size_t opt_off = coff_off + sizeof(COFFHeader);
    if (opt_off + 2 > data.size())
        throw std::runtime_error("Optional header out of bounds");
    uint16_t opt_magic = read_at<uint16_t>(data, opt_off);
    // 0x010B = PE32, 0x020B = PE32+
    if (opt_magic != 0x010B && opt_magic != 0x020B)
        throw std::runtime_error("Unknown optional header magic");

    // Data directory[0] (export table) is at different offsets in PE32 vs PE32+
    // PE32:  +96 from opt header start
    // PE32+: +112 from opt header start
    size_t export_dir_rva_off = opt_off + (opt_magic == 0x020B ? 112 : 96);
    if (export_dir_rva_off + 8 > data.size())
        throw std::runtime_error("Export data directory out of bounds");
    uint32_t export_rva  = read_at<uint32_t>(data, export_dir_rva_off);
    uint32_t export_size = read_at<uint32_t>(data, export_dir_rva_off + 4);

    if (export_rva == 0 || export_size == 0)
        return Pe32Info{{}, is_64};

    // Section headers follow the optional header
    size_t sections_off = opt_off + coff.opt_header_size;
    std::vector<SectionHeader> sections(coff.num_sections);
    for (size_t i = 0; i < coff.num_sections; ++i)
        sections[i] = read_at<SectionHeader>(data, sections_off + i * sizeof(SectionHeader));

    uint32_t export_file_off = rva_to_offset(export_rva, sections);
    ExportDirectory exp = read_at<ExportDirectory>(data, export_file_off);

    if (exp.num_names == 0)
        return Pe32Info{{}, is_64};

    // Read the array of name RVAs
    uint32_t names_file_off = rva_to_offset(exp.addr_of_names, sections);
    std::vector<std::string> exports;
    exports.reserve(exp.num_names);

    for (uint32_t i = 0; i < exp.num_names; ++i) {
        uint32_t name_rva = read_at<uint32_t>(data, names_file_off + i * 4);
        uint32_t name_off = rva_to_offset(name_rva, sections);
        // Read null-terminated string
        std::string name;
        while (name_off < data.size() && data[name_off] != '\0')
            name += static_cast<char>(data[name_off++]);
        exports.push_back(std::move(name));
    }

    return Pe32Info{std::move(exports), is_64};
}

// Fallback: invoke winedump and parse its output
Pe32Info parse_pe32_winedump(const fs::path& path) {
    auto run_winedump = [&](const std::vector<std::string>& extra_args) -> std::string {
        int pipefd[2];
        if (pipe(pipefd) != 0)
            throw std::runtime_error("pipe() failed");
        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            throw std::runtime_error("fork() failed");
        }
        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
            // Silence stderr
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);

            std::vector<const char*> argv = {"winedump"};
            for (const auto& a : extra_args)
                argv.push_back(a.c_str());
            argv.push_back(path.c_str());
            argv.push_back(nullptr);
            execvp("winedump", const_cast<char* const*>(argv.data()));
            _exit(127);
        }
        close(pipefd[1]);
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
            out.append(buf, n);
        close(pipefd[0]);
        int wstatus;
        waitpid(pid, &wstatus, 0);
        if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 127)
            throw std::runtime_error(
                "Could not find 'winedump'. In some distributions this is part of a separate Wine "
                "tools package.");
        return out;
    };

    // Determine machine type from basic info
    std::string basic = run_winedump({});
    bool is_64 = true;
    bool found_machine = false;
    std::istringstream bi(basic);
    std::string line;
    while (std::getline(bi, line)) {
        // Look for "  Machine:   014C ..."
        auto pos = line.find("Machine:");
        if (pos == std::string::npos)
            continue;
        std::string rest = line.substr(pos + 8);
        // Trim leading spaces
        size_t start = rest.find_first_not_of(" \t");
        if (start != std::string::npos)
            rest = rest.substr(start);
        is_64 = rest.substr(0, 4) != "014C";
        found_machine = true;
        break;
    }
    if (!found_machine)
        throw std::runtime_error(
            "Winedump output did not contain a 'Machine:' line. Is this a text file?");

    // Get exports
    std::string exp_out = run_winedump({"-j", "export"});
    std::vector<std::string> exports;
    bool in_table = false;
    std::istringstream ei(exp_out);
    while (std::getline(ei, line)) {
        std::string trimmed = line;
        // Trim leading whitespace
        size_t s = trimmed.find_first_not_of(" \t");
        if (s != std::string::npos)
            trimmed = trimmed.substr(s);
        else
            trimmed.clear();

        if (in_table) {
            if (trimmed.empty())
                break;
            // Format: "d34db33f 1 symbol_name"
            std::istringstream ls(trimmed);
            std::string a, b, sym;
            if (ls >> a >> b >> sym)
                exports.push_back(sym);
        } else if (trimmed.substr(0, 8) == "Entry Pt") {
            in_table = true;
        }
    }

    return Pe32Info{std::move(exports), is_64};
}

}  // namespace

Pe32Info parse_pe32_binary(const fs::path& path) {
    try {
        return parse_pe32_native(path);
    } catch (const std::exception& e) {
        try {
            return parse_pe32_winedump(path);
        } catch (const std::exception& we) {
            throw std::runtime_error(std::string("Failed to parse with both built-in parser and "
                                                 "winedump:\n  native: ") +
                                     e.what() + "\n  winedump: " + we.what());
        }
    }
}
