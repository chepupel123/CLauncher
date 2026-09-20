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
namespace {

std::mutex g_mutex;
fs::path g_log_file;                 // путь к clauncher.log
bool g_initialized = false;
std::vector<std::string> g_pending;  // сообщения до init()

// Каталог, где лежит исполняемый файл (для clauncher.log «рядом с .exe»).
fs::path exe_dir(const char* argv0) {
    try {
        if (argv0 && *argv0) {
            fs::path p(argv0);
            if (p.has_parent_path()) return p.parent_path();
        }
#ifdef _WIN32
        wchar_t buf[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) return fs::path(std::wstring(buf, n)).parent_path();
#else
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; return fs::path(buf).parent_path(); }
#endif
    } catch (...) {
    }
    return fs::path(".");
}

const char* level_name(Level l) {
    switch (l) {
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Error: return "ERROR";
    }
    return "INFO";
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

} // namespace

void init(const char* argv0) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_initialized = true;

    // 1) Рядом с exe (в т.ч. на Windows — где лежит cacert.pem).
    fs::path next_to_exe = exe_dir(argv0) / "clauncher.log";

    // 2) Папка данных лаунчера — запасной вариант, куда точно есть права записи.
    fs::path in_data_dir = launcher_paths::launcher_dir() / "clauncher.log";

    fs::path chosen = next_to_exe;
    std::error_code ec;
    {
        std::ofstream probe(next_to_exe, std::ios::app);
        if (!probe.is_open()) {
            // Нет прав писать рядом с exe (типично для Program Files).
            chosen = in_data_dir;
        }
    }

    g_log_file = chosen;
    {
        std::error_code mk;
        fs::create_directories(g_log_file.parent_path(), mk);
    }

    // Дописываем всё, что накопилось до init().
    std::ofstream file(g_log_file, std::ios::app);
    if (file.is_open()) {
        for (const auto& line : g_pending) file << line << "\n";
        file << "[" << timestamp() << "] [INFO] Log file: " << g_log_file.string() << "\n";
    }
    g_pending.clear();
}

void write(Level level, const std::string& message) {
    const std::string line = "[" + timestamp() + "] [" + level_name(level) + "] " + message;

    std::lock_guard<std::mutex> lk(g_mutex);

    if (g_initialized) {
        std::ofstream file(g_log_file, std::ios::app);
        if (file.is_open()) {
            file << line << "\n";
        }
    } else {
        // До init(): файла ещё нет — буферизуем (и всё равно показываем в консоль).
        g_pending.push_back(line);
    }

    if (level == Level::Error) {
        std::cerr << line << "\n";
    } else {
        std::cout << line << "\n";
    }
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
