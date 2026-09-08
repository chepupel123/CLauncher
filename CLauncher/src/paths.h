#pragma once
#include <filesystem>
#include <cstdlib>
#include <string>

// Cross-platform path helpers.
//
// Linux:   game   -> ~/.minecraft
//          launcher -> ~/.minecraft-launcher
// Windows: game   -> %APPDATA%\.minecraft   (matches the official launcher)
//          launcher -> %APPDATA%\minecraft-launcher
//
// %APPDATA% maps to ...\AppData\Roaming, which is where the official
// Minecraft launcher keeps .minecraft, so worlds/settings stay shared.

namespace launcher_paths {

inline std::filesystem::path home_dir() {
#ifdef _WIN32
    const char* profile = std::getenv("USERPROFILE");
    if (profile && *profile) return std::filesystem::path(profile);
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive && path) return std::filesystem::path(std::string(drive) + path);
    return std::filesystem::path(".");
#else
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) : std::filesystem::path(".");
#endif
}

inline std::filesystem::path minecraft_dir() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) return std::filesystem::path(appdata) / ".minecraft";
#endif
    return home_dir() / ".minecraft";
}

inline std::filesystem::path launcher_dir() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) return std::filesystem::path(appdata) / "minecraft-launcher";
    return home_dir() / "minecraft-launcher";
#else
    return home_dir() / ".minecraft-launcher";
#endif
}

} // namespace launcher_paths
