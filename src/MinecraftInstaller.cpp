#include "MinecraftInstaller.h"
#include "version_manager.h"
#include "JavaManager.h"
#include "paths.h"
#include "curl_tls.h"
#include "logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
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

#include "Downloader.h"
#include "ModLoaders.h"

using namespace downloader;

namespace MinecraftInstaller {
    std::atomic<bool> g_cancel_requested{false};

    void request_cancel() { g_cancel_requested.store(true); }
    bool is_cancelled() { return g_cancel_requested.load(); }
}

namespace {
    bool ensure_assets(const fs::path& minecraft, const json& metadata,
                       int pct_from = -1, int pct_to = -1)
    {
        if (!metadata.contains("assetIndex")) return true;

        const auto& ai = metadata["assetIndex"];
        std::string asset_index_url = ai["url"];
        std::string asset_index_id = ai["id"];
        std::string index_sha = ai.value("sha1", std::string());

        fs::path assets = minecraft / "assets";
        fs::path index_file = assets / "indexes" / (asset_index_id + ".json");

        bool index_ok = fs::exists(index_file) &&
                        (index_sha.empty() || verify_sha1(index_file, index_sha));
        if (!index_ok) {
            LOG_INFO("Downloading asset index...");
            if (!download(asset_index_url, index_file)) return false;
            if (!index_sha.empty() && !verify_sha1(index_file, index_sha)) {
                set_install_error("SHA1 mismatch: asset index " + asset_index_id);
                return false;
            }
        }

        json asset_index;
        try {
            std::ifstream file(index_file);
            if (!file) { set_install_error("Failed to open asset index"); return false; }
            file >> asset_index;
        } catch (const std::exception& e) {
            set_install_error(std::string("Failed to parse asset index: ") + e.what());
            return false;
        }

        if (!asset_index.contains("objects")) return true;



        std::vector<DownloadItem> items;
        items.reserve(asset_index["objects"].size());
        for (const auto& [name, object] : asset_index["objects"].items()) {
            std::string hash = object["hash"];
            std::string prefix = hash.substr(0, 2);
            DownloadItem it;
            it.url = "https://resources.download.minecraft.net/" + prefix + "/" + hash;
            it.dest = assets / "objects" / prefix / hash;
            it.sha1 = hash;
            it.label = name;
            it.size = json_file_size(object);
            items.push_back(std::move(it));
        }
        return parallel_download(std::move(items), "Assets", 16, pct_from, pct_to);
    }

    bool ensure_natives(const fs::path& version_dir, const json& metadata)
    {
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
            if (!downloads.contains("classifiers") ||
                !downloads["classifiers"].contains(native_classifier)) continue;
            auto native_info = downloads["classifiers"][native_classifier];
            std::string url = native_info["url"];
            std::string path = native_info["path"];
            std::string native_sha = native_info.value("sha1", std::string());
            fs::path dest_jar = version_dir / "libraries" / path;
            bool jar_ok = fs::exists(dest_jar) &&
                          (native_sha.empty() || verify_sha1(dest_jar, native_sha));
            if (!jar_ok) {
                LOG_INFO("Downloading natives: " << path);
                if (!download(url, dest_jar)) return false;
                if (!native_sha.empty() && !verify_sha1(dest_jar, native_sha)) {
                    LOG_WARN("Natives SHA1 mismatch, retrying: " << path);
                    if (!download(url, dest_jar)) return false;
                    if (!verify_sha1(dest_jar, native_sha)) {
                        set_install_error("Natives SHA1 mismatch persists: " + path);
                        return false;
                    }
                }
            }
            native_jars.push_back(dest_jar);
            std::string id = native_sha;
            if (id.empty() && !sha1_of_file(dest_jar, id))
                id = dest_jar.filename().string();
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
            while (std::getline(in, line)) {
                if (!line.empty()) saved.push_back(line);
            }
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
            set_install_error(
                "unzip is not installed (needed to extract Minecraft native libraries). "
                "Please install unzip.");
            return false;
        }
#endif
        for (const auto& jar : native_jars) {
            const auto jar_u8 = jar.u8string();
            const std::string jar_path_utf8(
                reinterpret_cast<const char*>(jar_u8.data()), jar_u8.size());
            const auto natives_dir_u8 = natives_dir.u8string();
            const std::string natives_path_utf8(
                reinterpret_cast<const char*>(natives_dir_u8.data()), natives_dir_u8.size());
#ifdef _WIN32
            std::string cmd = "tar -xf \"" + jar_path_utf8 +
                              "\" -C \"" + natives_path_utf8 + "\"";
#else
            std::string cmd = "unzip -o -q \"" + jar_path_utf8 +
                              "\" -d \"" + natives_path_utf8 + "\"";
#endif
            if (std::system(cmd.c_str()) != 0) {
#ifdef _WIN32
                set_install_error("Failed to extract natives (tar): " + jar_path_utf8);
#else
                set_install_error("Failed to extract natives (is unzip installed?): " +
                                  jar_path_utf8);
#endif
                return false;
            }
        }
        std::ofstream marker_out(marker);
        if (marker_out) {
            for (const auto& line : jar_ids) marker_out << line << '\n';
        }
        return true;
    }

}

bool MinecraftInstaller::install(const std::string& version)
{
    g_cancel_requested.store(false);
    reset_speed_state();
    g_last_error.clear();
    try
    {
        fs::path minecraft = launcher_paths::shared_dir();
        fs::path version_dir = minecraft / "versions" / version;
        fs::path version_json = version_dir / (version + ".json");
        fs::path client_jar = version_dir / (version + ".jar");

        bool fresh = false;
        json metadata;

        if (fs::exists(version_json) && fs::exists(client_jar) &&
            version_complete(version_dir)) {


            LOG_INFO("Minecraft " << version << " is already downloaded, verifying...");
            try {
                std::ifstream file(version_json);
                if (!file) throw std::runtime_error("cannot open");
                file >> metadata;
            } catch (const std::exception& e) {
                LOG_WARN("version.json corrupted (" << e.what() << "), re-downloading");
                fresh = true;
            }
        } else {
            fresh = true;
        }

        if (fresh) {
            auto versions = VersionManager::parse_manifest();
            std::string version_url;
            for (const auto& item : versions) {
                if (item.id == version) { version_url = item.url; break; }
            }
            if (version_url.empty()) {
                set_install_error("Version not found in manifest: " + version);
                return false;
            }

            report_progress(2, "Version metadata...");
            LOG_INFO("Downloading Minecraft " << version << " metadata...");
            fs::create_directories(version_dir);
            metadata = download_json(version_url);
            {
                std::ofstream file(version_json);
                if (!file) {
                    set_install_error("Failed to write " + version_json.string());
                    return false;
                }
                file << metadata.dump(2);
            }

            if (!metadata.contains("downloads") || !metadata["downloads"].contains("client")) {
                set_install_error("Client download information is missing");
                return false;
            }
            std::string client_url = metadata["downloads"]["client"]["url"];
            std::string client_sha = metadata["downloads"]["client"].value("sha1", std::string());
            report_progress(6, "Downloading client...");
            LOG_INFO("Downloading Minecraft client...");
            if (!download(client_url, client_jar)) return false;
            if (!client_sha.empty() && !verify_sha1(client_jar, client_sha)) {
                LOG_WARN("Client SHA1 mismatch, retrying download...");
                if (!download(client_url, client_jar)) return false;
                if (!verify_sha1(client_jar, client_sha)) {
                    set_install_error("Client SHA1 mismatch persists");
                    return false;
                }
            }
        }






        {
            std::string client_sha;
            if (metadata.contains("downloads") && metadata["downloads"].contains("client"))
                client_sha = metadata["downloads"]["client"].value("sha1", std::string());
            if (!client_sha.empty() && !verify_sha1(client_jar, client_sha)) {
                report_progress(4, "Re-downloading client...");
                LOG_WARN("Client JAR corrupted (SHA1 mismatch), re-downloading...");
                std::string client_url = metadata["downloads"]["client"]["url"];
                if (!download(client_url, client_jar)) return false;
                if (!verify_sha1(client_jar, client_sha)) {
                    set_install_error("Client JAR re-download failed");
                    return false;
                }
            }
        }
        if (!ensure_libraries(version_dir, metadata, 10, 45)) {
            if (g_last_error.empty()) set_install_error("Failed to download libraries");
            return false;
        }
        if (!ensure_assets(minecraft, metadata, 45, 92)) {
            if (g_last_error.empty()) set_install_error("Failed to download assets");
            return false;
        }
        report_progress(92, "Natives...");
        if (!ensure_natives(version_dir, metadata)) {
            if (g_last_error.empty()) set_install_error("Failed to install natives");
            return false;
        }
        report_progress(98, "Done");

        mark_version_complete(version_dir);
        LOG_INFO("Minecraft " << version << " installed successfully");
        return true;
    }
    catch (const std::exception& e) {
        set_install_error(std::string("MinecraftInstaller error: ") + e.what());
        return false;
    }
}

bool MinecraftInstaller::installPerformanceMods(const fs::path& game_dir,
                                                const std::string& mc_version)
{
    g_cancel_requested.store(false);
    reset_speed_state();
    try {
        fs::path mods_dir = game_dir / "mods";
        fs::create_directories(mods_dir);


        fs::path manifest_path = mods_dir / ".launcher-mods.json";
        json manifest = json::object();
        if (fs::exists(manifest_path)) {
            try {
                std::ifstream mf(manifest_path);
                mf >> manifest;
            } catch (...) {
                LOG_WARN("mods manifest corrupted, starting fresh");
                manifest = json::object();
            }
        }

        const std::pair<std::string, std::string> mods[] = {
            {"Fabric API",  "fabric-api"},
            {"Sodium",      "sodium"},
            {"Lithium",     "lithium"},
            {"FerriteCore", "ferrite-core"},
        };

        std::vector<DownloadItem> items;
        json new_manifest = json::object();
        for (const auto& [title, slug] : mods) {

            std::string api_url = "https://api.modrinth.com/v2/project/" + slug +
                "/version?game_versions=%5B%22" + mc_version + "%22%5D&loaders=%5B%22fabric%22%5D";
            json versions = download_json(api_url);
            if (!versions.is_array() || versions.empty()) {
                LOG_ERROR("Modrinth: no build of " << title << " for Minecraft "
                          << mc_version);
                return false;
            }

            const json* latest = &versions[0];
            std::string latest_date = versions[0].value("date_published", std::string());
            for (const auto& ver : versions) {
                std::string date = ver.value("date_published", std::string());
                if (date > latest_date) {
                    latest = &ver;
                    latest_date = std::move(date);
                }
            }
            const json& v = *latest;
            const json* file = nullptr;
            if (v.contains("files") && v["files"].is_array()) {
                for (const auto& f : v["files"]) {
                    if (f.value("primary", false)) { file = &f; break; }
                }
                if (!file && !v["files"].empty()) file = &v["files"][0];
            }
            if (!file || !file->contains("url")) {
                LOG_ERROR("Modrinth: malformed response for " << title);
                return false;
            }

            DownloadItem it;
            it.url = (*file)["url"].get<std::string>();
            it.dest = mods_dir / file->value("filename", slug + ".jar");
            if (file->contains("hashes") && (*file)["hashes"].contains("sha1"))
                it.sha1 = (*file)["hashes"]["sha1"].get<std::string>();
            it.label = title + " " + v.value("version_number", std::string());


            std::string old_file = manifest.value(slug, std::string());
            if (!old_file.empty() && old_file != it.dest.filename().string()) {
                std::error_code ec;
                fs::remove(mods_dir / old_file, ec);
                if (!ec) LOG_INFO("Removed old version of " << title << ": " << old_file);
            }
            new_manifest[slug] = it.dest.filename().string();
            items.push_back(std::move(it));
        }

        if (!parallel_download(std::move(items), "Mods", 16, 45, 95)) return false;

        std::ofstream mf(manifest_path);
        mf << new_manifest.dump(2);
        LOG_INFO("Performance mods installed: Fabric API, Sodium, Lithium, FerriteCore");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Mods install error: " << e.what());
        return false;
    }
}
