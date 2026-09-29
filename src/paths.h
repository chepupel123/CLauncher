#pragma once

#include <filesystem>
#include <cstdlib>
#include <string>

// Cross-platform path helpers.
//
// Linux:    game -> ~/.minecraft,  launcher -> ~/.minecraft-launcher
// Windows:  game -> %APPDATA%\.minecraft, launcher -> %APPDATA%\minecraft-launcher
//
// Uses SHGetKnownFolderPath on Windows to handle non-ASCII usernames.

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

} // namespace launcher_paths#pragma once

#include <filesystem>
#include <cstdlib>
#include <string>

// Cross-platform path helpers.
//
// Linux: game -> ~/.minecraft
// launcher -> ~/.minecraft-launcher
// Windows: game -> %APPDATA%\.minecraft (matches the official launcher)
// launcher -> %APPDATA%\minecraft-launcher
//
// %APPDATA% maps to ...\AppData\Roaming, which is where the official
// Minecraft launcher keeps .minecraft, so worlds/settings stay shared.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#endif

namespace launcher_paths {

// Internal helper: get a known folder path on Windows via SHGetKnownFolderPath
// Returns empty path on failure.
inline std::filesystem::path get_known_folder_path(const GUID& folder_id) {
#ifdef _WIN32
    PWSTR path = nullptr;
    HRESULT hr = SHGetKnownFolderPath(folder_id, 0, nullptr, &path);
    if (SUCCEEDED(hr) && path) {
        std::filesystem::path result(path);
        CoTaskMemFree(path);
        return result;
    }
    return std::filesystem::path();
#else
    (void)folder_id;
    return std::filesystem::path();
#endif
}

inline std::filesystem::path home_dir() {
#ifdef _WIN32
    // Try known folder: FOLDERID_Profile (C:\Users\<name>)
    std::filesystem::path known = get_known_folder_path(FOLDERID_Profile);
    if (!known.empty()) return known;

    // Fallback: USERPROFILE env var (ANSI, but usually works)
    const char* profile = std::getenv("USERPROFILE");
    if (profile && *profile) return std::filesystem::path(profile);

    // Fallback: HOMEDRIVE + HOMEPATH
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive && path) return std::filesystem::path(std::string(drive) + path);

    return std::filesystem::path(".");
#else
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) : std::filesystem::path(".");
#endif
}

inline std::filesystem::path appdata_dir() {
#ifdef _WIN32
    // Use SHGetKnownFolderPath for proper Unicode support (handles non-ASCII usernames)
    std::filesystem::path known = get_known_folder_path(FOLDERID_RoamingAppData);
    if (!known.empty()) return known;

    // Fallback: APPDATA env var (ANSI, may break on non-ASCII usernames)
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) return std::filesystem::path(appdata);

    // Final fallback: home_dir() + "AppData\Roaming"
    return home_dir() / "AppData" / "Roaming";
#else
    // On Linux/macOS, XDG_CONFIG_HOME or ~/.config
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

// New helper: directory for logs (same as launcher_dir)
inline std::filesystem::path log_dir() {
    return launcher_dir();
}

} // namespace launcher_paths
