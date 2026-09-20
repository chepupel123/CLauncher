#include "version_scanner.h"
#include "paths.h"
#include <filesystem>
#include <iostream>
#include <algorithm>

namespace fs = std::filesystem;

std::string VersionScanner::get_versions_path() {
    return (launcher_paths::minecraft_dir() / "versions").string();
}

std::vector<std::string> VersionScanner::scan_versions() {
    std::vector<std::string> versions;
    auto versions_dir = get_versions_path();

    try {
        if (!fs::exists(versions_dir)) {
            std::cerr << "Versions dir not found: " << versions_dir << "\n";
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

        std::cout << "Found " << versions.size() << " Minecraft versions\n";

    } catch (const std::exception& e) {
        std::cerr << "Version scan error: " << e.what() << "\n";
    }

    return versions;
}
