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

#include "util.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "config.h"

static constexpr const char* VSTBRIDGE_HOST_EXPECTED_PREFIX = "Usage: vstbridge-";
static constexpr const char* LIBDBUS_NAME = "libdbus-1.so.3";
static constexpr const char* LIBDBUS_FALLBACK = "libdbus-1.so";

LibArchitecture get_elf_architecture(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Could not open '" + path.string() + "'");

    // e_ident[EI_DATA] at offset 0x05: 1 = little-endian, 2 = big-endian
    file.seekg(0x05);
    uint8_t endian_byte{};
    file.read(reinterpret_cast<char*>(&endian_byte), 1);
    bool little_endian = (endian_byte == 1);

    // e_machine at offset 0x12: 2 bytes
    file.seekg(0x12);
    uint8_t mach[2]{};
    file.read(reinterpret_cast<char*>(mach), 2);

    uint16_t machine = little_endian ? static_cast<uint16_t>(mach[0] | (mach[1] << 8))
                                     : static_cast<uint16_t>((mach[0] << 8) | mach[1]);

    if (machine == 0x03)
        return LibArchitecture::Lib32;
    if (machine == 0x3E)
        return LibArchitecture::Lib64;

    throw std::runtime_error("'" + path.string() + "' is not a recognized ELF machine ISA");
}

std::optional<NativeFile> get_file_type(const fs::path& path) {
    std::error_code ec;
    auto st = fs::symlink_status(path, ec);
    if (ec)
        return std::nullopt;

    if (fs::is_symlink(st))
        return NativeFile::make_symlink(path);
    if (fs::is_directory(st))
        return NativeFile::make_directory(path);
    if (fs::is_regular_file(st))
        return NativeFile::make_regular(path);
    return std::nullopt;
}

LibArchitecture get_default_wine_prefix_arch() {
    const char* home_env = std::getenv("HOME");
    if (!home_env)
        return LibArchitecture::Lib64;

    fs::path reg_path = fs::path(home_env) / ".wine" / "system.reg";
    std::ifstream reg(reg_path);
    if (!reg)
        return LibArchitecture::Lib64;

    std::string line;
    while (std::getline(reg, line)) {
        if (line == "#arch=win32")
            return LibArchitecture::Lib32;
        if (line == "#arch=win64")
            break;
    }
    return LibArchitecture::Lib64;
}

// FNV-1a hash — deterministic and stable across implementations
int64_t hash_file(const fs::path& path) {
    auto bytes = read_bytes(path);
    uint64_t h = 14695981039346656037ULL;
    for (uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return static_cast<int64_t>(h);
}

fs::path normalize_path(const fs::path& path) {
    // Walk up the path until we find a prefix that can be canonicalized
    for (auto it = path.begin(); it != path.end(); ++it) {
        fs::path prefix;
        for (auto jt = path.begin(); jt != it; ++jt)
            prefix /= *jt;
        prefix /= *it;

        std::error_code ec;
        auto canonical = fs::canonical(prefix, ec);
        if (!ec) {
            // Build the rest of the path on top of the canonical prefix
            fs::path rest;
            auto jt = it;
            ++jt;
            for (; jt != path.end(); ++jt)
                rest /= *jt;
            return canonical / rest;
        }
    }
    return path;
}

void copy_or_reflink(const fs::path& from, const fs::path& to) {
#ifdef FICLONE
    int src = open(from.c_str(), O_RDONLY);
    if (src >= 0) {
        int dst = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst >= 0) {
            bool ok = (ioctl(dst, FICLONE, src) == 0);
            close(src);
            close(dst);
            if (ok)
                return;
            // reflink failed — fall through to regular copy
            ::remove(to.c_str());
        } else {
            close(src);
        }
    }
#endif
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error("Error copying '" + from.string() + "' to '" + to.string() +
                                 "': " + ec.message());
}

void create_dir_all(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec)
        throw std::runtime_error("Error creating directories for '" + path.string() +
                                 "': " + ec.message());
}

void remove_dir_all(const fs::path& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec)
        throw std::runtime_error("Could not remove directory '" + path.string() +
                                 "': " + ec.message());
}

void remove_file_or_dir(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
    if (ec)
        throw std::runtime_error("Could not remove '" + path.string() + "': " + ec.message());
}

void make_symlink(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_symlink(target, link, ec);
    if (ec)
        throw std::runtime_error("Error symlinking '" + target.string() + "' to '" +
                                 link.string() + "': " + ec.message());
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Could not write to '" + path.string() + "'");
    out << contents;
}

std::string read_to_string(const fs::path& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("Could not read file '" + path.string() + "'");
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Could not read file '" + path.string() + "'");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

// Run a command and return its combined stdout output, or throw on failure
static std::string run_command_output(const std::string& cmd,
                                      const std::vector<std::string>& args,
                                      bool clear_env = false,
                                      const std::string& home_env = {}) {
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
        // Child
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (clear_env) {
            clearenv();
            if (!home_env.empty())
                setenv("HOME", home_env.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(cmd.c_str()));
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execvp(cmd.c_str(), argv.data());
        _exit(127);
    }

    // Parent
    close(pipefd[1]);
    std::string output;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        output.append(buf, n);
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);
    return output;
}

static int run_command_status(const std::string& cmd, const std::vector<std::string>& args,
                              bool clear_env, const std::string& home_env,
                              const std::string& argv0_override = {}) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        if (clear_env) {
            clearenv();
            if (!home_env.empty())
                setenv("HOME", home_env.c_str(), 1);
        }
        // Redirect stdout/stderr to /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        std::vector<char*> argv;
        std::string argv0 = argv0_override.empty() ? cmd : argv0_override;
        argv.push_back(const_cast<char*>(argv0.c_str()));
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        execvp(cmd.c_str(), argv.data());
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool verify_path_setup() {
    // First check ~/.local/share/vstbridge directly (vstbridge always searches there)
    try {
        auto xdg_home = vstbridge_data_home();
        auto host_exe = xdg_home / VSTBRIDGE_HOST_EXE_NAME;
        if (fs::exists(host_exe)) {
            if (access(host_exe.c_str(), X_OK) == 0)
                return true;
            std::cerr << "\n"
                      << wrap_text("WARNING: '" + host_exe.string() +
                                   "' exists but is not executable. Something probably went wrong "
                                   "when extracting vstbridge's files.")
                      << "\n";
            return false;
        }
    } catch (...) {
    }

    const char* shell_env = std::getenv("SHELL");
    if (!shell_env) {
        std::cerr << "\nWarning: Could not determine login shell, skipping the PATH setup check\n";
        return true;
    }

    std::string shell_path = shell_env;
    std::string shell = fs::path(shell_path).filename().string();

    std::vector<std::string> args;
    std::string argv0 = "-" + shell_path;

    if (shell == "ash" || shell == "bash" || shell == "csh" || shell == "ksh" ||
        shell == "dash" || shell == "fish" || shell == "ion" || shell == "sh" ||
        shell == "tcsh" || shell == "zsh") {
        args = {"-l", "-c",
                std::string("command -v ") + VSTBRIDGE_HOST_EXE_NAME};
    } else if (shell == "elvish" || shell == "oil") {
        args = {"-c", std::string("command -v ") + VSTBRIDGE_HOST_EXE_NAME};
    } else if (shell == "pwsh") {
        args = {"-l", "-c", std::string("which ") + VSTBRIDGE_HOST_EXE_NAME};
    } else if (shell == "nu") {
        args = {"-c", std::string("which ") + VSTBRIDGE_HOST_EXE_NAME};
    } else {
        std::cerr << "\n"
                  << wrap_text("WARNING: Yabridgectl does not know how to handle your login shell "
                               "'" +
                               shell +
                               "', skipping the PATH environment variable check. Feel free to open "
                               "a feature request in order to get vstbridgectl to support your "
                               "shell.\n\nhttps://github.com/rations/vstbridge/issues")
                  << "\n";
        return true;
    }

    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    int rc = run_command_status(shell_path, args, true, home, argv0);
    if (rc == 0)
        return true;

    if (rc == 127) {
        std::cerr << "\n"
                  << wrap_text("Warning: could not run " + shell +
                               " as a login shell, skipping the PATH setup check")
                  << "\n";
        return true;
    }

    std::cerr << "\n"
              << wrap_text("Warning: '" + std::string(VSTBRIDGE_HOST_EXE_NAME) +
                           "' is not present in either '~/.local/share/vstbridge' your login "
                           "shell's search path. You may not have extracted vstbridge's files to "
                           "the correct location. See the readme's usage section for more details. "
                           "Rerun this command to verify that the variable has been set "
                           "correctly.\n\nhttps://github.com/rations/vstbridge#usage")
              << "\n";
    return false;
}

void verify_wine_setup(Config& config) {
    const char* wine_loader = std::getenv("WINELOADER");
    std::string wine_bin = wine_loader ? wine_loader : "wine";

    // Unset WAYLAND_DISPLAY like vstbridge itself does
    unsetenv("WAYLAND_DISPLAY");

    std::string wine_out;
    try {
        wine_out = run_command_output(wine_bin, {"--version"});
    } catch (...) {
        throw std::runtime_error("Could not run '" + wine_bin +
                                 "', make sure Wine is installed");
    }

    // Strip trailing newline
    while (!wine_out.empty() && (wine_out.back() == '\n' || wine_out.back() == '\r'))
        wine_out.pop_back();
    if (wine_out.empty())
        throw std::runtime_error("Running '" + wine_bin + " --version' resulted in empty output");

    auto files = config.files();

    auto exe_so = files.vstbridge_host_exe_so ? files.vstbridge_host_exe_so
                                             : files.vstbridge_host_32_exe_so;
    if (!exe_so)
        throw std::runtime_error("Could not locate '" + std::string(VSTBRIDGE_HOST_EXE_NAME) +
                                 ".so'");

    int64_t host_hash = hash_file(*exe_so);

    KnownConfig current{wine_out, host_hash};
    if (config.last_known_config && config.last_known_config->wine_version == current.wine_version &&
        config.last_known_config->vstbridge_host_hash == current.vstbridge_host_hash)
        return;

    // Pick the right host binary based on Wine prefix architecture
    fs::path host_path;
    if (get_default_wine_prefix_arch() == LibArchitecture::Lib32) {
        if (!files.vstbridge_host_32_exe)
            throw std::runtime_error("Could not find '" +
                                     std::string(VSTBRIDGE_HOST_32_EXE_NAME) + "'");
        host_path = *files.vstbridge_host_32_exe;
    } else {
        if (!files.vstbridge_host_exe)
            throw std::runtime_error("Could not find '" + std::string(VSTBRIDGE_HOST_EXE_NAME) +
                                     "'");
        host_path = *files.vstbridge_host_exe;
    }

    std::string stderr_out;
    try {
        stderr_out = run_command_output(host_path.string(), {});
    } catch (...) {
        throw std::runtime_error("Could not run '" + host_path.string() + "'");
    }

    bool success = false;
    std::string last_error;
    std::istringstream iss(stderr_out);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.substr(0, std::strlen(VSTBRIDGE_HOST_EXPECTED_PREFIX)) ==
            VSTBRIDGE_HOST_EXPECTED_PREFIX) {
            success = true;
            break;
        }
        // Ignore fixme lines
        if (line.size() >= 10 && line.substr(5, 5) != "fixme")
            last_error = line;
    }

    if (success) {
        config.last_known_config = current;
        config.write();
    } else {
        std::string wine_ver = wine_out;
        if (wine_ver.substr(0, 5) == "wine-")
            wine_ver = wine_ver.substr(5);

        std::cerr << "\n"
                  << wrap_text(
                         "Warning: Could not run '" + std::string(VSTBRIDGE_HOST_EXE_NAME) +
                         "'. Wine reported the following error:\n\n" +
                         (last_error.empty() ? "<no output>" : last_error) +
                         "\n\nMake sure that you have downloaded the correct version of vstbridge "
                         "for your distro.\nThis can also happen when using a version of Wine "
                         "that's not compatible with this version of vstbridge, in which case "
                         "you'll need to upgrade Wine. Your current Wine version is '" +
                         wine_ver +
                         "'. See the link below for instructions on how to upgrade your "
                         "installation of Wine.\n\nhttps://github.com/robbert-vdh/"
                         "vstbridge#troubleshooting-common-issues")
                  << "\n";
    }
}

void verify_external_dependencies() {
    void* handle = dlopen(LIBDBUS_NAME, RTLD_LAZY);
    if (!handle)
        handle = dlopen(LIBDBUS_FALLBACK, RTLD_LAZY);
    if (handle) {
        dlclose(handle);
        return;
    }
    std::cerr << "\n"
              << wrap_text(std::string("Warning: Could not find '") + LIBDBUS_NAME +
                           "'. This will not prevent vstbridge from working, but you will also not "
                           "receive any notifications when something is wrong. D-Bus should "
                           "already be installed on the system, so if you're seeing this warning "
                           "then something is probably very wrong.")
              << "\n";
}

std::string wrap_text(const std::string& text) {
    struct winsize ws{};
    unsigned int width = 80;
    if (isatty(STDERR_FILENO) && ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        width = ws.ws_col;

    std::string result;
    std::istringstream para_iss(text);
    std::string line;
    bool first_para_line = true;

    while (std::getline(para_iss, line)) {
        if (line.empty()) {
            result += "\n";
            first_para_line = true;
            continue;
        }
        // Word-wrap this line, indenting continuation lines with 4 spaces
        std::istringstream word_iss(line);
        std::string word;
        unsigned int col = 0;
        bool first_word = true;

        while (word_iss >> word) {
            unsigned int needed = first_word ? word.size() : word.size() + 1;
            if (!first_word && col + needed > width) {
                result += "\n    ";
                col = 4;
                first_word = true;
            } else if (!first_word) {
                result += " ";
                col++;
            }
            result += word;
            col += word.size();
            first_word = false;
        }
        result += "\n";
        first_para_line = false;
    }

    // Remove trailing newline
    while (!result.empty() && result.back() == '\n')
        result.pop_back();

    return result;
}

namespace color {

bool enabled() {
    static bool val = isatty(STDOUT_FILENO);
    return val;
}

std::string red(const std::string& s) {
    return enabled() ? "\x1b[31m" + s + "\x1b[0m" : s;
}
std::string green(const std::string& s) {
    return enabled() ? "\x1b[32m" + s + "\x1b[0m" : s;
}
std::string yellow(const std::string& s) {
    return enabled() ? "\x1b[33m" + s + "\x1b[0m" : s;
}
std::string cyan(const std::string& s) {
    return enabled() ? "\x1b[36m" + s + "\x1b[0m" : s;
}
std::string magenta(const std::string& s) {
    return enabled() ? "\x1b[35m" + s + "\x1b[0m" : s;
}
std::string bright_white(const std::string& s) {
    return enabled() ? "\x1b[97m" + s + "\x1b[0m" : s;
}

}  // namespace color
