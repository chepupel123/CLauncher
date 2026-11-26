#include "MinecraftInstaller.h"
#include "version_manager.h"
#include "JavaManager.h"
#include "paths.h"
#include "logger.h"
#include "Downloader.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace downloader;
namespace downloader {
    bool ensure_assets(const fs::path& minecraft, const json& metadata, int pct_from, int pct_to);
    
    extern MinecraftInstaller::ProgressFn g_progress_fn;
    extern void* g_progress_user;
    extern int g_stage_pct_from;
    extern int g_stage_pct_to;
    extern std::string g_last_error; // На всякий случай, если нет в Downloader.h
}
// =========================================================================

namespace MinecraftInstaller {
    std::atomic<bool> g_cancel_requested{false};
    void request_cancel() { g_cancel_requested.store(true); }
    bool is_cancelled() { return g_cancel_requested.load(); }
}

namespace {
    bool ensure_natives(const fs::path& version_dir, const json& metadata) {
        fs::path natives_dir = version_dir / "natives";
        fs::create_directories(natives_dir);
#if defined(_WIN32)
        const std::string native_classifier = "natives-windows";
#elif defined(__APPLE__)
        const std::string native_classifier = "natives-osx";
#else
        const std::string native_classifier = "natives-linux";
#endif
        std::vector<fs::path> native_jars;
        std::vector<std::string> jar_ids;
        for (const auto& library : metadata["libraries"]) {
            if (!library_allowed(library)) continue;
            if (!library.contains("downloads")) continue;
            const auto& downloads = library["downloads"];
            if (!downloads.contains("classifiers") || !downloads["classifiers"].contains(native_classifier)) continue;
            auto native_info = downloads["classifiers"][native_classifier];
            std::string url = native_info["url"];
            std::string path = native_info["path"];
            std::string native_sha = native_info.value("sha1", std::string());
            fs::path dest_jar = version_dir / "libraries" / path;
            native_jars.push_back(dest_jar);
            std::string id = !native_sha.empty() ? native_sha : path;
            jar_ids.push_back(id + " " + dest_jar.filename().string());
        }
        if (native_jars.empty()) return true;
        std::sort(jar_ids.begin(), jar_ids.end());
        fs::path marker = natives_dir / ".extracted-jars";
        bool marker_ok = false;
        if (fs::exists(marker)) {
            std::vector<std::string> saved;
            std::ifstream in(marker);
            std::string line;
            while (std::getline(in, line)) { if (!line.empty()) saved.push_back(line); }
            std::sort(saved.begin(), saved.end());
            marker_ok = (saved == jar_ids);
        }
        bool natives_have_files = false;
        for (const auto& entry : fs::directory_iterator(natives_dir)) {
            if (entry.path().filename() == ".extracted-jars") continue;
            natives_have_files = true;
            break;
        }
        if (marker_ok && natives_have_files) return true;

#ifndef _WIN32
        if (std::system("command -v unzip >/dev/null 2>&1") != 0) {
            set_install_error("unzip is not installed.");
            return false;
        }
#endif
        for (size_t i = 0; i < native_jars.size(); ++i) {
            const auto& jar = native_jars[i];
            std::string native_sha, url;
            for (const auto& library : metadata["libraries"]) {
                if (!library_allowed(library) || !library.contains("downloads")) continue;
                const auto& downloads = library["downloads"];
                if (!downloads.contains("classifiers") || !downloads["classifiers"].contains(native_classifier)) continue;
                auto info = downloads["classifiers"][native_classifier];
                if (version_dir / "libraries" / info["path"].get<std::string>() == jar) {
                    url = info["url"]; native_sha = info.value("sha1", ""); break;
                }
            }
            bool jar_ok = fs::exists(jar) && (native_sha.empty() || verify_sha1(jar, native_sha));
            if (!jar_ok) {
                if (!download(url, jar)) return false;
                if (!native_sha.empty() && !verify_sha1(jar, native_sha)) { set_install_error("Natives SHA1 mismatch persists: " + jar.filename().string()); return false; }
            }
            const auto jar_u8 = jar.u8string();
            const std::string jar_path_utf8(reinterpret_cast<const char*>(jar_u8.data()), jar_u8.size());
            const auto natives_dir_u8 = natives_dir.u8string();
            const std::string natives_path_utf8(reinterpret_cast<const char*>(natives_dir_u8.data()), natives_dir_u8.size());
#ifdef _WIN32
            std::string cmd = "tar -xf \"" + jar_path_utf8 + "\" -C \"" + natives_path_utf8 + "\"";
#else
            std::string cmd = "unzip -o -q \"" + jar_path_utf8 + "\" -d \"" + natives_path_utf8 + "\"";
#endif
            if (std::system(cmd.c_str()) != 0) { set_install_error("Failed to extract natives: " + jar_path_utf8); return false; }
        }
        std::ofstream marker_out(marker);
        if (marker_out) { for (const auto& line : jar_ids) marker_out << line << '\n'; }
        return true;
    }
}

bool MinecraftInstaller::install(const std::string& version) {
    g_cancel_requested.store(false);
    reset_speed_state();
    g_last_error.clear();
    try {
        fs::path minecraft = launcher_paths::shared_dir();
        fs::path version_dir = minecraft / "versions" / version;
        fs::path version_json = version_dir / (version + ".json");
        fs::path client_jar = version_dir / (version + ".jar");

        if (fs::exists(version_json) && fs::exists(client_jar) && version_complete(version_dir)) {
            LOG_INFO("Minecraft " << version << " is already installed. Fast launching...");
            report_progress(100, "Already installed");
            return true;
        }

        bool fresh = false;
        json metadata;
        if (fs::exists(version_json) && fs::exists(client_jar)) {
            try {
                std::ifstream file(version_json);
                if (!file) throw std::runtime_error("cannot open");
                file >> metadata;
                std::string client_sha;
                if (metadata.contains("downloads") && metadata["downloads"].contains("client"))
                    client_sha = metadata["downloads"]["client"].value("sha1", std::string());
                if (!client_sha.empty() && !verify_sha1(client_jar, client_sha)) fresh = true;
            } catch (const std::exception& e) { fresh = true; }
        } else { fresh = true; }

        if (fresh) {
            auto versions = VersionManager::parse_manifest();
            std::string version_url;
            for (const auto& item : versions) { if (item.id == version) { version_url = item.url; break; } }
            if (version_url.empty()) { set_install_error("Version not found in manifest: " + version); return false; }

            report_progress(2, "Version metadata...");
            fs::create_directories(version_dir);
            metadata = download_json(version_url);
            { std::ofstream file(version_json); if (!file) { set_install_error("Failed to write " + version_json.string()); return false; } file << metadata.dump(2); }

            std::string client_url = metadata["downloads"]["client"]["url"];
            std::string client_sha = metadata["downloads"]["client"].value("sha1", std::string());
            report_progress(6, "Downloading client...");
            if (!download(client_url, client_jar)) return false;
            if (!client_sha.empty() && !verify_sha1(client_jar, client_sha)) { set_install_error("Client SHA1 mismatch persists"); return false; }
        }

        if (!ensure_libraries(version_dir, metadata, 10, 45)) { if (g_last_error.empty()) set_install_error("Failed to download libraries"); return false; }
        
        // Теперь эта функция явно объявлена в начале файла
        if (!downloader::ensure_assets(minecraft, metadata, 45, 92)) { if (g_last_error.empty()) set_install_error("Failed to download assets"); return false; }
        
        report_progress(92, "Natives...");
        if (!ensure_natives(version_dir, metadata)) { if (g_last_error.empty()) set_install_error("Failed to install natives"); return false; }
        
        report_progress(98, "Done");
        mark_version_complete(version_dir);
        LOG_INFO("Minecraft " << version << " installed successfully");
        return true;
    } catch (const std::exception& e) {
        set_install_error(std::string("MinecraftInstaller error: ") + e.what());
        return false;
    }
}

bool MinecraftInstaller::supportsPerformanceMods(const std::string& mc_version) {
    if (mc_version.rfind("26.", 0) == 0) return true;
    int major = 0, minor = 0, patch = 0;
    if (std::sscanf(mc_version.c_str(), "%d.%d.%d", &major, &minor, &patch) < 2) return false;
    if (major != 1) return major > 1;
    if (minor != 16) return minor > 16;
    return patch >= 4;
}

void MinecraftInstaller::set_progress_callback(ProgressFn fn, void* user) {
    g_progress_fn = fn;
    g_progress_user = user;
}

int MinecraftInstaller::bar_percent(size_t bytes_done, size_t bytes_total) {
    if (bytes_total == 0) return 0;
    if (bytes_done > bytes_total) bytes_done = bytes_total;
    double frac = static_cast<double>(bytes_done) / static_cast<double>(bytes_total);
    if (g_stage_pct_from >= 0 && g_stage_pct_to > g_stage_pct_from)
        return g_stage_pct_from + static_cast<int>((g_stage_pct_to - g_stage_pct_from) * frac);
    return static_cast<int>(100.0 * frac);
}

std::string MinecraftInstaller::last_error() { return g_last_error; }