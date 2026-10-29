#pragma once

#include <filesystem>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#endif

namespace launcher_paths {

#ifdef _WIN32
inline std::filesystem::path get_known_folder_path(const GUID& folder_id) {
    PWSTR path = nullptr;
    HRESULT hr = SHGetKnownFolderPath(folder_id, 0, nullptr, &path);
    if (SUCCEEDED(hr) && path) {
        std::filesystem::path result(path);
        CoTaskMemFree(path);
        return result;
    }
    return std::filesystem::path();
}
#endif

inline std::filesystem::path home_dir() {
#ifdef _WIN32
    std::filesystem::path known = get_known_folder_path(FOLDERID_Profile);
    if (!known.empty()) return known;

    const char* profile = std::getenv("USERPROFILE");
    if (profile && *profile) return std::filesystem::path(profile);

    const char* drive = std::getenv("HOMEDRIVE");
    const char* path  = std::getenv("HOMEPATH");
    if (drive && path) return std::filesystem::path(std::string(drive) + path);

    return std::filesystem::path(".");
#else
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home)
                         : std::filesystem::path(".");
#endif
}

inline std::filesystem::path appdata_dir() {
#ifdef _WIN32
    std::filesystem::path known = get_known_folder_path(FOLDERID_RoamingAppData);
    if (!known.empty()) return known;

    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) return std::filesystem::path(appdata);

    return home_dir() / "AppData" / "Roaming";
#else
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) return std::filesystem::path(xdg_config);
    return home_dir() / ".config";
#endif
}

// ---------------------------------------------------------------------------
// Legacy: единый глобальный .minecraft. Больше не используется новым кодом
// (инстансы дают свой --gameDir), но нужен для обратной совместимости —
// в частности, для legacy-перегрузки JavaLauncher::launch(nick, version, ...).
// ---------------------------------------------------------------------------
inline std::filesystem::path minecraft_dir() {
#ifdef _WIN32
    return appdata_dir() / ".minecraft";
#else
    return home_dir() / ".minecraft";
#endif
}

inline std::filesystem::path launcher_dir() {
#ifdef _WIN32
    return appdata_dir() / "CLauncher";
#else
    return home_dir() / ".clauncher";
#endif
}

// ---------------------------------------------------------------------------
// Общие ресурсы всех инстансов (read-only после установки, кроме установщика).
//   shared/assets/{indexes,objects}
//   shared/libraries/<group>/<artifact>/<version>/<file>.jar
//   shared/versions/<version_id>/{<id>.json, <id>.jar, natives, .complete}
//   shared/runtime/{java8,java17,java21}
// ---------------------------------------------------------------------------
inline std::filesystem::path shared_dir() {
    return launcher_dir() / "shared";
}

inline std::filesystem::path shared_assets_dir() {
    return shared_dir() / "assets";
}

inline std::filesystem::path shared_libraries_dir() {
    return shared_dir() / "libraries";
}

inline std::filesystem::path shared_versions_dir() {
    return shared_dir() / "versions";
}

// Новое место для JRE. JavaManager пока использует legacy runtime_dir(),
// переключение — отдельным шагом (потребует миграции скачанных JRE).
inline std::filesystem::path shared_runtime_dir() {
    return shared_dir() / "runtime";
}

// ---------------------------------------------------------------------------
// Инстансы: каждый — своя изолированная сборка.
//   instances/<name>/instance.json
//   instances/<name>/.minecraft/{mods,saves,resourcepacks,config,...}
// ---------------------------------------------------------------------------
inline std::filesystem::path instances_dir() {
    return launcher_dir() / "instances";
}

// ---------------------------------------------------------------------------
// Legacy / общие служебные директории
// ---------------------------------------------------------------------------
// Legacy: старый путь для JRE. Оставлен, пока JavaManager не перейдёт
// на shared_runtime_dir(). Не использовать в новом коде.
inline std::filesystem::path runtime_dir() {
    return launcher_dir() / "runtime";
}

inline std::filesystem::path cache_dir() {
    return launcher_dir() / "cache";
}

inline std::filesystem::path log_dir() {
    return launcher_dir() / "logs";
}

} // namespace launcher_paths
