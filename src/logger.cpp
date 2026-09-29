#include "logger.h"
#include "paths.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace logger {

// ====== helpers (internal) ======
namespace {

std::mutex      g_mutex;
fs::path        g_log_file;
#ifdef _WIN32
FILE*           g_file = nullptr;
#else
std::ofstream   g_file;
#endif
bool            g_initialized = false;
std::vector<std::string> g_pending;

fs::path exe_dir(const char* argv0) {
    try {
        if (argv0 && *argv0) {
            fs::path p(argv0);
            if (p.has_parent_path()) return p.parent_path();
        }
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            return fs::path(std::wstring(buf, n)).parent_path();
#else
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; return fs::path(buf).parent_path(); }
#endif
    } catch (...) {}
    return fs::path(".");
}

const char* level_name(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "INFO ";
}

std::string timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

void open_file_locked() {
#ifdef _WIN32
    // "ab" — binary append: без CRLF-конверсии, без порчи UTF-8.
    // НЕ используем "ccs=UTF-8": MinGW его не поддерживает.
    g_file = _wfopen(g_log_file.wstring().c_str(), L"ab");
#else
    g_file.open(g_log_file, std::ios::app);
#endif
}

void write_line_locked(const std::string& line) {
#ifdef _WIN32
    if (g_file) {
        fputs(line.c_str(), g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
#else
    if (g_file.is_open()) {
        g_file << line << "\n";
        g_file.flush();
    }
#endif
}

} // anonymous namespace

// ====== global state (external linkage) ======
Level g_min_level = Level::Info;

void set_min_level(Level level) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_min_level = level;
}

Level min_level() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_min_level;
}

// ====== public API ======
void init(const char* argv0) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_initialized) return;
    g_initialized = true;

    fs::path next_to_exe = exe_dir(argv0) / "clauncher.log";
    fs::path in_data_dir = launcher_paths::launcher_dir() / "clauncher.log";
    fs::path cwd_log     = fs::current_path() / "clauncher.log";

    auto can_write = [](const fs::path& p) -> bool {
#ifdef _WIN32
        FILE* probe = _wfopen(p.wstring().c_str(), L"ab");
        if (!probe) return false;
        fclose(probe);
        return true;
#else
        std::ofstream probe(p, std::ios::app);
        return probe.is_open();
#endif
    };

    fs::path chosen = next_to_exe;
    if (!can_write(chosen)) chosen = in_data_dir;
    if (!can_write(chosen)) chosen = cwd_log;

    g_log_file = chosen;

    std::error_code mk;
    fs::create_directories(g_log_file.parent_path(), mk);
    open_file_locked();

#ifdef _WIN32
    if (g_file) {
        for (const auto& line : g_pending) {
            fputs(line.c_str(), g_file);
            fputc('\n', g_file);
        }
        std::string init_line =
            "[" + timestamp() + "] [INFO ] Log file: " + g_log_file.string();
        fputs(init_line.c_str(), g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
#else
    if (g_file.is_open()) {
        for (const auto& line : g_pending) g_file << line << "\n";
        g_file << "[" << timestamp() << "] [INFO ] Log file: "
               << g_log_file.string() << "\n";
        g_file.flush();
    }
#endif
    g_pending.clear();
}

void write(Level level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(g_min_level)) return;

    const std::string line =
        "[" + timestamp() + "] [" + level_name(level) + "] " + message;

    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_initialized) {
        write_line_locked(line);
    } else {
        g_pending.push_back(line);
    }

#ifndef _WIN32
    // На Linux консоль полезна. На Windows GUI её нет.
    if (level == Level::Error) std::cerr << line << "\n";
    else                       std::cout << line << "\n";
#endif
}

std::string log_file_path() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_log_file.string();
}

std::string log_directory() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_log_file.parent_path().string();
}

} // namespace logger
