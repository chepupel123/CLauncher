#include "single_instance.h"
#include "logger.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
#include <filesystem>
#endif

namespace single_instance {

#ifdef _WIN32

static HANDLE g_mutex = nullptr;

bool acquire() {
    const wchar_t* name = L"Global\\CLauncher_SingleInstance_Mutex";
    g_mutex = CreateMutexW(nullptr, TRUE, name);
    if (g_mutex == nullptr) {
        LOG_ERROR("CreateMutexW failed, GetLastError=" << GetLastError());
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) {
        LOG_WARN("Another CLauncher instance is already running");
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        return false;
    }
    LOG_INFO("Single-instance lock acquired (Windows)");
    return true;
}

void release() {
    if (g_mutex) {
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
}

#else

static int g_lock_fd = -1;

bool acquire() {
    std::string path;
    const char* home = std::getenv("HOME");
    if (home && *home) {
        path = std::string(home) + "/.minecraft-launcher/.clauncher.lock";
    } else {
        path = "/tmp/clauncher.lock";
    }
    try {
        std::filesystem::path p(path);
        std::filesystem::create_directories(p.parent_path());
    } catch (...) {}

    g_lock_fd = open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (g_lock_fd < 0) {
        LOG_WARN("Cannot open lock file " << path << " — allowing launch");
        return true;
    }
    if (flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        LOG_WARN("Another CLauncher instance is already running");
        close(g_lock_fd);
        g_lock_fd = -1;
        return false;
    }
    LOG_INFO("Single-instance lock acquired (Linux)");
    return true;
}

void release() {
    if (g_lock_fd >= 0) {
        flock(g_lock_fd, LOCK_UN);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}

#endif

} // namespace single_instance
