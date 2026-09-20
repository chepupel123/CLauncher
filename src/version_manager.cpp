#include "version_manager.h"
#include "curl_tls.h"
#include "logger.h"
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

// Единственная точка отсечки неподдерживаемых линеек.
//
// Поддерживается только ветка 1.x (последняя — 1.21.x). Линейка 26.x и новее
// сознательно вырезана: она требует Java 25 и нового Loom, экосистема Fabric
// под неё ещё не готова, а игра нацелена на заметно более новое железо, чем то,
// под которое заточен лаунчер. Если версия не проходит этот фильтр, она не
// попадает ни в список версий, ни в какой-либо последующий код:
// ни в выбор Java, ни в установку Fabric/модов, ни в запуск игры.
static bool is_supported_release(const std::string& id) {
    long major = 0;
    size_t i = 0;
    bool has_digit = false;
    while (i < id.size() && id[i] >= '0' && id[i] <= '9') {
        major = major * 10 + (id[i] - '0');
        ++i;
        has_digit = true;
    }
    return has_digit && major == 1;
}

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
    auto ftime = fs::last_write_time(path);
#if defined(_MSC_VER)
    auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
#else
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
#endif
    return std::chrono::system_clock::to_time_t(sctp);
}

bool VersionManager::fetch_and_cache_manifest() {
    if (has_manifest_cache()) {
        const long age = std::time(nullptr) - get_cache_timestamp();
        if (age >= 0 && age < MANIFEST_TTL_SECONDS) {
            LOG_INFO("Manifest cache is fresh, skipping download");
            return true;
        }
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to initialize curl");
        return false;
    }

    // TLS: на Windows — NATIVE_CA либо cacert.pem рядом с exe; на Linux — системные CA.
    // Если ни один источник не настроен, apply() вернёт false — тогда HTTPS
    // заведомо упадёт, и мы честно логируем это (а не выдаём непонятную ошибку).
    if (!curl_tls::apply(curl)) {
        LOG_WARN("curl TLS source is not configured; HTTPS download will likely fail");
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

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (res == CURLE_SSL_CACERT || res == CURLE_PEER_FAILED_VERIFICATION) {
            LOG_ERROR("curl TLS error: " << curl_easy_strerror(res));
#if defined(_WIN32)
            LOG_ERROR("Windows CA detection failed. Expected chain: native CA store "
                      "(CURLSSLOPT_NATIVE_CA) or cacert.pem next to the .exe "
                      "(via CURLOPT_CAINFO).");
#else
            LOG_ERROR("CA verification failed on Linux too — check the ca-certificates package.");
#endif
        } else {
            LOG_ERROR("Curl failed: " << curl_easy_strerror(res)
                      << " (HTTP code: " << http_code << ")");
        }
        LOG_WARN("No internet connection. Using cached manifest if available.");
        return false;
    }

    if (http_code != 200) {
        LOG_ERROR("Manifest download failed: HTTP " << http_code);
        return false;
    }

    try {
        auto manifest = json::parse(response);
        if (!manifest.contains("versions")) {
            LOG_ERROR("Invalid manifest format: no 'versions' field");
            return false;
        }

        auto cache_path = get_manifest_cache_path();
        std::ofstream cache_file(cache_path);
        if (!cache_file.is_open()) {
            LOG_ERROR("Failed to write cache to " << cache_path);
            return false;
        }

        cache_file << response;
        cache_file.close();

        LOG_INFO("Manifest cached to " << cache_path);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("JSON parse error: " << e.what());
        return false;
    }
}

std::vector<VersionManager::Version> VersionManager::parse_manifest() {
    std::vector<Version> versions;
    auto cache_path = get_manifest_cache_path();

    if (!fs::exists(cache_path)) {
        LOG_ERROR("Manifest cache not found at " << cache_path);
        return versions;
    }

    try {
        std::ifstream cache_file(cache_path);
        if (!cache_file.is_open()) {
            LOG_ERROR("Failed to open cache file");
            return versions;
        }

        auto manifest = json::parse(cache_file);
        cache_file.close();

        if (!manifest.contains("versions")) {
            LOG_ERROR("Invalid manifest: no 'versions' field");
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

        LOG_INFO("Parsed " << versions.size() << " versions from manifest");
        return versions;

    } catch (const std::exception& e) {
        LOG_ERROR("Error parsing manifest: " << e.what());
        return versions;
    }
}

std::vector<std::string> VersionManager::get_release_versions() {
    std::vector<std::string> release_versions;
    for (const auto& v : parse_manifest())
        if (v.type == "release" && is_supported_release(v.id))
            release_versions.push_back(v.id);

    LOG_INFO("Found " << release_versions.size() << " release versions");
    return release_versions;
}

bool VersionManager::has_release_versions() {
    return !get_release_versions().empty();
}
