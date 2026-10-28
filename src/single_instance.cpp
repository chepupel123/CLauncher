#include "single_instance.h"
#include "logger.h"

#include <string>
#include <filesystem>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace single_instance {

#ifdef _WIN32

static HANDLE g_mutex = nullptr;

bool acquire() {
    const wchar_t* name = L"Local\\CLauncher_SingleInstance_Mutex";
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
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    std::filesystem::path lock_dir;
    
    if (runtime_dir && std::filesystem::exists(runtime_dir)) {
        lock_dir = runtime_dir;
    } else {
        lock_dir = std::filesystem::temp_directory_path();
    }

    std::filesystem::path lock_path = lock_dir / "clauncher.lock";
    std::string path_str = lock_path.string();

    g_lock_fd = open(path_str.c_str(), O_RDWR | O_CREAT, 0644);
    if (g_lock_fd < 0) {
        LOG_WARN("Cannot open lock file " << path_str << " — allowing launch");
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

InstanceLock::InstanceLock() {
    m_acquired = acquire();
}

InstanceLock::~InstanceLock() {
    if (m_acquired) {
        release();
    }
}

} // namespace single_instance