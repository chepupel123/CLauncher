#include "version_manager.h"
#include "paths.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;
using json = nlohmann::json;

static constexpr long MANIFEST_TTL_SECONDS = 3600;

size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string VersionManager::get_manifest_cache_path() {
    fs::path dir = launcher_paths::launcher_dir();
    fs::create_directories(dir);
    return (dir / "versions_manifest.json").string();
}

bool VersionManager::has_manifest_cache() {
    return fs::exists(get_manifest_cache_path());
}

long VersionManager::get_cache_timestamp() {
    auto path = get_manifest_cache_path();
    if (!fs::exists(path)) return 0;
    // Portable conversion: clock_cast handles both libstdc++ (file_time_type ==
    // system_clock) and MSVC (file_time_type == file_clock).
    auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(
        fs::last_write_time(path));
    return std::chrono::system_clock::to_time_t(sctp);
}

bool VersionManager::fetch_and_cache_manifest() {
    if (has_manifest_cache()) {
        const long age = std::time(nullptr) - get_cache_timestamp();
        if (age >= 0 && age < MANIFEST_TTL_SECONDS) {
            std::cout << "Manifest cache is fresh, skipping download\n";
            return true;
        }
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize curl\n";
        return false;
    }

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "Curl error: " << curl_easy_strerror(res) << "\n";
        std::cerr << "No internet connection. Using cached manifest if available.\n";
        return false;
    }

    try {
        auto manifest = json::parse(response);
        if (!manifest.contains("versions")) {
            std::cerr << "Invalid manifest format: no 'versions' field\n";
            return false;
        }

        auto cache_path = get_manifest_cache_path();
        std::ofstream cache_file(cache_path);
        if (!cache_file.is_open()) {
            std::cerr << "Failed to write cache to " << cache_path << "\n";
            return false;
        }

        cache_file << response;
        cache_file.close();

        std::cout << "Manifest cached to " << cache_path << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << "\n";
        return false;
    }
}

std::vector<VersionManager::Version> VersionManager::parse_manifest() {
    std::vector<Version> versions;
    auto cache_path = get_manifest_cache_path();

    if (!fs::exists(cache_path)) {
        std::cerr << "Manifest cache not found at " << cache_path << "\n";
        return versions;
    }

    try {
        std::ifstream cache_file(cache_path);
        if (!cache_file.is_open()) {
            std::cerr << "Failed to open cache file\n";
            return versions;
        }

        auto manifest = json::parse(cache_file);
        cache_file.close();

        if (!manifest.contains("versions")) {
            std::cerr << "Invalid manifest: no 'versions' field\n";
            return versions;
        }

        for (const auto& v : manifest["versions"]) {
            versions.push_back(Version{
                .id = v.value("id", "unknown"),
                .type = v.value("type", "unknown"),
                .url = v.value("url", ""),
                .sha1 = v.value("sha1", ""),
                .time = v.value("time", ""),
                .releaseTime = v.value("releaseTime", "")
            });
        }

        std::cout << "Parsed " << versions.size() << " versions from manifest\n";
        return versions;

    } catch (const std::exception& e) {
        std::cerr << "Error parsing manifest: " << e.what() << "\n";
        return versions;
    }
}

std::vector<std::string> VersionManager::get_release_versions() {
    std::vector<std::string> release_versions;
    for (const auto& v : parse_manifest())
        if (v.type == "release") release_versions.push_back(v.id);

    std::cout << "Found " << release_versions.size() << " release versions\n";
    return release_versions;
}
