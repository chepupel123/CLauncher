#pragma once

#include <filesystem>
#include <string>
#include "mod_loader.h"

class Instance {
public:
    // Идентификация
    std::string           name;
    std::filesystem::path root_path;     // launcher_dir()/instances/<name>

    // Конфигурация
    std::string mc_version;              // "1.20.1"
    std::string version_id;              // "fabric-loader-0.15.11-1.20.1" | "1.20.1"
    ModLoader   mod_loader  = ModLoader::Vanilla;
    int         memory_mb   = 2048;
    std::string java_path;               // пусто = авто
    std::string nickname;

    // Производные пути (gameDir изолирован)
    std::filesystem::path game_dir()            const { return root_path / ".minecraft"; }
    std::filesystem::path mods_dir()            const { return game_dir() / "mods"; }
    std::filesystem::path saves_dir()           const { return game_dir() / "saves"; }
    std::filesystem::path resource_packs_dir()  const { return game_dir() / "resourcepacks"; }
    std::filesystem::path config_dir()          const { return game_dir() / "config"; }
    std::filesystem::path instance_json()       const { return root_path / "instance.json"; }

    bool load(const std::filesystem::path& path);
    bool save() const;
    bool ensure_layout() const;
};
