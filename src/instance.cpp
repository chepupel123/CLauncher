#include "instance.h"
#include "logger.h"

#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static const char* modloader_to_str(ModLoader m) {
    return m == ModLoader::Fabric ? "fabric" : "vanilla";
}
static ModLoader modloader_from_str(const std::string& s) {
    return s == "fabric" ? ModLoader::Fabric : ModLoader::Vanilla;
}

bool Instance::load(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) { LOG_ERROR("instance: cannot open " + path.string()); return false; }
    json j;
    try { in >> j; } catch (const std::exception& e) {
        LOG_ERROR(std::string("instance: bad json: ") + e.what());
        return false;
    }
    try {
        root_path  = path.parent_path();
        name       = j.value("name", root_path.filename().string());
        mc_version = j.value("mc_version", "");
        version_id = j.value("version_id", mc_version);
        mod_loader = modloader_from_str(j.value("mod_loader", "vanilla"));
        memory_mb  = j.value("memory_mb", 2048);
        java_path  = j.value("java_path", "");
        nickname   = j.value("nickname", "");
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("instance: field error: ") + e.what());
        return false;
    }
    if (version_id.empty()) version_id = mc_version;
    return true;
}

bool Instance::save() const {
    json j = {
        {"name",       name},
        {"mc_version", mc_version},
        {"version_id", version_id},
        {"mod_loader", modloader_to_str(mod_loader)},
        {"memory_mb",  memory_mb},
        {"java_path",  java_path},
        {"nickname",   nickname},
    };
    std::ofstream out(instance_json());
    if (!out) { LOG_ERROR("instance: cannot write " + instance_json().string()); return false; }
    out << j.dump(2) << '\n';
    return out.good();
}

bool Instance::ensure_layout() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(mods_dir(), ec);
    fs::create_directories(saves_dir(), ec);
    fs::create_directories(resource_packs_dir(), ec);
    fs::create_directories(config_dir(), ec);
    if (ec) { LOG_ERROR("instance: mkdir failed: " + ec.message()); return false; }
    return true;
}
