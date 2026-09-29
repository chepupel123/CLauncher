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

inline std::filesystem::path minecraft_dir() {
#ifdef _WIN32
    return appdata_dir() / ".minecraft";
#else
    return home_dir() / ".minecraft";
#endif
}

inline std::filesystem::path launcher_dir() {
#ifdef _WIN32
    return appdata_dir() / "minecraft-launcher";
#else
    return home_dir() / ".minecraft-launcher";
#endif
}

inline std::filesystem::path log_dir() {
    return launcher_dir();
}

} // namespace launcher_paths
