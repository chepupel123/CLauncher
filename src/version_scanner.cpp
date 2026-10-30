#include "version_scanner.h"
#include "paths.h"
#include "logger.h"
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

std::string VersionScanner::get_versions_path() {
    return launcher_paths::shared_versions_dir().string();
}

std::vector<std::string> VersionScanner::scan_versions() {
    std::vector<std::string> versions;
    auto versions_dir = get_versions_path();

    try {
        if (!fs::exists(versions_dir)) {
            LOG_WARN("Versions dir not found: " << versions_dir);
            return versions;
        }

        for (const auto& entry : fs::directory_iterator(versions_dir)) {
            if (entry.is_directory()) {
                auto version_name = entry.path().filename().string();

                auto json_path = entry.path() / (version_name + ".json");
                if (fs::exists(json_path)) {
                    versions.push_back(version_name);
                }
            }
        }

        std::sort(versions.begin(), versions.end());
        std::reverse(versions.begin(), versions.end());

        LOG_INFO("Found " << versions.size() << " Minecraft versions");

    } catch (const std::exception& e) {
        LOG_ERROR("Version scan error: " << e.what());
    }

    return versions;
}
