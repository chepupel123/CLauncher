#include "MinecraftInstaller.h"
#include "version_manager.h"
#include "JavaManager.h"
#include "paths.h"
#include "curl_tls.h"
#include "logger.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

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

namespace {
    bool maven_relative_path(const std::string& name, std::string& out)
    {
        std::vector<std::string> parts;
        size_t start = 0, end = 0;
        while ((end = name.find(':', start)) != std::string::npos) {
            parts.push_back(name.substr(start, end - start));
            start = end + 1;
        }
        parts.push_back(name.substr(start));
        if (parts.size() < 3 || parts[0].empty() || parts[1].empty() || parts[2].empty()) return false;

        std::string group = parts[0];
        std::replace(group.begin(), group.end(), '.', '/');
        out = group + "/" + parts[1] + "/" + parts[2] + "/" +
              parts[1] + "-" + parts[2] +
              (parts.size() > 3 && !parts[3].empty() ? "-" + parts[3] : "") + ".jar";
        return true;
    }


    std::string pick_error_line(const std::string& text)
    {
        std::string last, hit;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            line = line.substr(a);
            last = line;
            if (line.find("Exception") != std::string::npos ||
                line.find("Error") != std::string::npos ||
                line.find("error") != std::string::npos ||
                line.find("Failed") != std::string::npos ||
                line.find("failed") != std::string::npos ||
                line.find("No launcher") != std::string::npos)
                hit = line;
        }
        std::string chosen = !hit.empty() ? hit : last;
        for (char& c : chosen) {
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        }
        if (chosen.size() > 240) chosen = chosen.substr(0, 237) + "...";
        return chosen;
    }

    void log_output_lines(const char* prefix, const std::string& text)
    {
        std::istringstream in(text);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            lines.push_back(line);
        }
        size_t start = lines.size() > 40 ? lines.size() - 40 : 0;
        if (start > 0)
            LOG_INFO(prefix << " (" << (lines.size() - start)
                            << " last lines, earlier omitted)");
        for (size_t i = start; i < lines.size(); ++i)
            LOG_INFO(prefix << lines[i]);
    }

    // NeoForge for 1.20.1 installs into "1.20.1-forge-47.1.x", the same id scheme
    // as Forge. Tell them apart by the profile: NeoForge lists net.neoforged.* libraries.
    bool is_neoforge_profile(const fs::path& profile_json)
    {
        std::ifstream in(profile_json);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str().find("net.neoforged") != std::string::npos;
    }
}

std::string MinecraftInstaller::findFabricDir(const std::string& mc_version)
{
    fs::path versions_dir = launcher_paths::shared_versions_dir();
    std::error_code ec;
    if (!fs::exists(versions_dir)) return "";

    const std::string suffix = "-" + mc_version;
    std::string best;
    fs::file_time_type best_time{};

    for (const auto& entry : fs::directory_iterator(versions_dir)) {
        if (!entry.is_directory()) continue;
        if (!version_complete(entry.path())) continue;
        std::string name = entry.path().filename().string();
        if (name.find("fabric-loader-") != 0) continue;
        if (name.size() < suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        auto t = entry.last_write_time(ec);
        if (ec) continue;
        if (best.empty() || t > best_time) { best = name; best_time = t; }
    }
    return best;
}

std::string MinecraftInstaller::installFabric(const std::string& version)
{
    g_cancel_requested.store(false);
    fs::path fabric_temp_dir;
    try {
        fs::path minecraft_dir = launcher_paths::shared_dir();
        std::string existing_fabric_dir = findFabricDir(version);
        if (!existing_fabric_dir.empty()) return existing_fabric_dir;

        if (!install(version)) {
            if (g_last_error.empty())
                set_install_error("Failed to install Vanilla " + version);
            return "";
        }


        fs::path installer_path = launcher_paths::cache_dir() / "fabric-installer.jar";
        fs::path launcher_dir = launcher_paths::cache_dir();
        if (!fs::exists(installer_path)) {
            LOG_INFO("Downloading Fabric Installer...");
            fs::create_directories(launcher_dir);
            std::string url = "https://maven.fabricmc.net/net/fabricmc/fabric-installer/1.0.1/fabric-installer-1.0.1.jar";
            if (!download(url, installer_path)) {
                if (g_last_error.empty())
                    set_install_error("Failed to download Fabric Installer");
                return "";
            }
        }


        std::string loader_version = FabricVersionMap::getLoaderForVersion(version);
        auto is_safe_token = [](const std::string& s) {
            if (s.empty()) return false;
            for (char c : s) {
                if (!(std::isalnum((unsigned char)c) || c == '.' ||
                      c == '_' || c == '-' || c == '+'))
                    return false;
            }
            return true;
        };
        if (!is_safe_token(version) || !is_safe_token(loader_version)) {
            set_install_error("Invalid version/loader string");
            return "";
        }
        LOG_INFO("Using Fabric loader " << loader_version);


        std::string java_cmd = "java";
        try {
            JavaManager jm;
            fs::path jp = jm.ensureJava(jm.requiredMajor(version));
            if (!jp.empty()) {
                const auto java_path_u8 = jp.u8string();
                java_cmd.assign(reinterpret_cast<const char*>(java_path_u8.data()),
                                java_path_u8.size());
            }
        } catch (...) {}

        fabric_temp_dir = minecraft_dir / ".fabric-staging";
        std::error_code staging_ec;
        fs::remove_all(fabric_temp_dir, staging_ec);
        if (staging_ec)
            throw std::runtime_error("Failed to clear Fabric staging directory: " + staging_ec.message());
        fs::create_directories(fabric_temp_dir, staging_ec);
        if (staging_ec)
            throw std::runtime_error("Failed to create Fabric staging directory: " + staging_ec.message());

        report_progress(35, "Installing Fabric...");
        LOG_INFO("Installing Fabric...");
        fs::path fabric_log = launcher_paths::cache_dir() / "fabric-installer.log";
        fs::create_directories(fabric_log.parent_path());
        const auto installer_path_u8 = installer_path.u8string();
        const std::string installer_path_utf8(
            reinterpret_cast<const char*>(installer_path_u8.data()), installer_path_u8.size());
        const auto fabric_temp_dir_u8 = fabric_temp_dir.u8string();
        const std::string fabric_temp_dir_utf8(
            reinterpret_cast<const char*>(fabric_temp_dir_u8.data()), fabric_temp_dir_u8.size());
        const auto fabric_log_u8 = fabric_log.u8string();
        const std::string fabric_log_path_utf8(
            reinterpret_cast<const char*>(fabric_log_u8.data()), fabric_log_u8.size());
        std::string cmd = "\"" + java_cmd + "\" -jar \"" + installer_path_utf8 +
                          "\" client -dir \"" + fabric_temp_dir_utf8 +
                          "\" -mcversion " + version +
                          " -loader " + loader_version +
                          " -noprofile" +
                          " > \"" + fabric_log_path_utf8 + "\" 2>&1";
        int status = std::system(cmd.c_str());
        std::string fabric_out;
        {
            std::ifstream in(fabric_log);
            std::ostringstream ss;
            ss << in.rdbuf();
            fabric_out = ss.str();
        }
        if (status != 0) {
            log_output_lines("Fabric installer: ", fabric_out);
            std::string line = pick_error_line(fabric_out);
            if (line.empty())
                line = "Fabric installation failed (status " + std::to_string(status) + ")";
            set_install_error(line);
            std::error_code cleanup_ec;
            fs::remove_all(fabric_temp_dir, cleanup_ec);
            return "";
        }

        fs::path staged_versions_dir = fabric_temp_dir / "versions";
        std::string found_folder = "fabric-loader-" + loader_version + "-" + version;
        fs::path fabric_dir = staged_versions_dir / found_folder;
        if (!fs::is_directory(fabric_dir)) {
            set_install_error("Fabric directory not found");
            std::error_code cleanup_ec;
            fs::remove_all(fabric_temp_dir, cleanup_ec);
            return "";
        }




        fs::path target_jar = fabric_dir / (found_folder + ".jar");
        if (fs::exists(target_jar)) {
            LOG_WARN("found phantom " << target_jar
                     << " (this file must NOT exist — it caused"
                     << " IllegalStateException: trying to load FabricLoaderImpl...)."
                     << " Removing.");
            std::error_code ec;
            fs::remove(target_jar, ec);
        }


        fs::path profile_json = fabric_dir / (found_folder + ".json");
        if (!fs::exists(profile_json)) {
            set_install_error("Fabric profile JSON not found: " + profile_json.string());
            std::error_code cleanup_ec;
            fs::remove_all(fabric_temp_dir, cleanup_ec);
            return "";
        }



        try {
            json fabric_profile;
            std::ifstream pf(profile_json);
            pf >> fabric_profile;
            if (fabric_profile.contains("libraries")) {
                for (const auto& lib : fabric_profile["libraries"]) {
                    if (!lib.contains("name")) continue;
                    std::string rel;
                    if (!maven_relative_path(lib["name"].get<std::string>(), rel)) continue;
                    fs::path dest = minecraft_dir / "libraries" / rel;
                    if (fs::exists(dest)) continue;
                    if (fs::exists(fabric_dir / "libraries" / rel)) continue;
                    if (fs::exists(minecraft_dir / "versions" / version / "libraries" / rel)) continue;
                    std::string base = lib.value("url", "https://maven.fabricmc.net/");
                    if (!base.empty() && base.back() != '/') base += '/';
                    LOG_INFO("Downloading Fabric library: " << rel);
                    if (!download(base + rel, dest)) {
                        if (g_last_error.empty())
                            set_install_error("Failed to download Fabric library: " + rel);
                        std::error_code cleanup_ec;
                        fs::remove_all(fabric_temp_dir, cleanup_ec);
                        return "";
                    }
                }
            }
        } catch (const std::exception& e) {
            set_install_error(std::string("Fabric profile verification failed: ") + e.what());
            std::error_code cleanup_ec;
            fs::remove_all(fabric_temp_dir, cleanup_ec);
            return "";
        }

        fs::path installed_fabric_dir = minecraft_dir / "versions" / found_folder;
        std::error_code pre_ec;
        fs::remove_all(installed_fabric_dir, pre_ec);
        std::error_code replace_ec;
        fs::rename(fabric_dir, installed_fabric_dir, replace_ec);
        if (replace_ec)
            throw std::runtime_error("Failed to install Fabric version: " + replace_ec.message());
        std::error_code cleanup_ec;
        fs::remove_all(fabric_temp_dir, cleanup_ec);

        mark_version_complete(installed_fabric_dir);
        report_progress(40, "Fabric installed");
        LOG_INFO("Fabric installed: " << found_folder);
        return found_folder;
    } catch (const std::exception& e) {
        if (!fabric_temp_dir.empty()) {
            std::error_code cleanup_ec;
            fs::remove_all(fabric_temp_dir, cleanup_ec);
        }
        set_install_error(std::string("Fabric install error: ") + e.what());
        return "";
    }
}

std::string MinecraftInstaller::installForge(const std::string& mc_version)
{
    g_cancel_requested.store(false);
    reset_speed_state();
    g_last_error.clear();

    try {
        if (mc_version.empty()) {
            set_install_error("Empty Minecraft version for Forge");
            return "";
        }

        const fs::path versions_dir = launcher_paths::shared_versions_dir();
        const std::string id_prefix = mc_version + "-forge-";

        std::error_code ec;
        if (fs::exists(versions_dir, ec)) {
            for (const auto& entry : fs::directory_iterator(versions_dir, ec)) {
                if (ec || !entry.is_directory()) continue;
                const std::string id = entry.path().filename().string();
                if (id.rfind(id_prefix, 0) != 0) continue;
                if (!version_complete(entry.path())) continue;
                if (!fs::is_regular_file(entry.path() / (id + ".json"))) continue;
                if (is_neoforge_profile(entry.path() / (id + ".json"))) continue;
                return id;
            }
        }

        const json prism_index = download_json(
            "https://meta.prismlauncher.org/v1/net.minecraftforge/index.json");

        std::string forge_version;
        std::string raw_prism_version;

        if (prism_index.contains("versions") && prism_index["versions"].is_array()) {
            for (const auto& v : prism_index["versions"]) {
                if (!v.contains("requires") || !v["requires"].is_array()) continue;
                if (!v.contains("version") || !v["version"].is_string()) continue;

                bool match = false;
                for (const auto& req : v["requires"]) {
                    if (!req.is_object()) continue;
                    std::string uid = req.value("uid", "");
                    std::string equals_val = req.value("equals", "");
                    if (uid == "net.minecraft" && equals_val == mc_version) {
                        match = true;
                        break;
                    }
                }

                if (match) {
                    raw_prism_version = v["version"].get<std::string>();
                    size_t dash_pos = raw_prism_version.find('-');
                    if (dash_pos != std::string::npos)
                        forge_version = raw_prism_version.substr(dash_pos + 1);
                    else
                        forge_version = raw_prism_version;
                    break;
                }
            }
        }

        if (forge_version.empty()) {
            set_install_error("No Forge build found for Minecraft " + mc_version);
            return "";
        }

        if (!install(mc_version)) {
            if (g_last_error.empty())
                set_install_error("Failed to install Vanilla " + mc_version);
            return "";
        }
        if (is_cancelled()) return "";

        const std::string expected_id = id_prefix + forge_version;
        const fs::path installed_dir = versions_dir / expected_id;
        std::error_code mk_ec;
        fs::create_directories(installed_dir, mk_ec);
        if (mk_ec) {
            set_install_error("Failed to create Forge directory: " + mk_ec.message());
            return "";
        }

        json forge_metadata = download_json(
            "https://meta.prismlauncher.org/v1/net.minecraftforge/" + raw_prism_version + ".json");

        if (is_version_less(mc_version, 1, 13, 0)
            && forge_metadata.contains("libraries")
            && forge_metadata["libraries"].is_array()) {

            auto force_universal_suffix = [](std::string& s) {
                if (s.size() < 4) return;
                if (s.compare(s.size() - 4, 4, ".jar") != 0) return;
                if (s.find("-universal.jar") != std::string::npos) return;
                s = s.substr(0, s.size() - 4) + "-universal.jar";
            };

            for (auto& lib : forge_metadata["libraries"]) {
                if (!lib.contains("name") || !lib["name"].is_string()) continue;
                std::string name = lib["name"].get<std::string>();

                if (name.find("net.minecraftforge:forge:") != std::string::npos
                    && name.find(":universal") == std::string::npos) {
                    name += ":universal";
                    lib["name"] = name;
                }

                const bool is_forge_universal =
                    name.find("net.minecraftforge:forge:") != std::string::npos
                    && name.find(":universal") != std::string::npos;

                const bool has_artifact =
                    lib.contains("downloads") && lib["downloads"].is_object()
                    && lib["downloads"].contains("artifact")
                    && lib["downloads"]["artifact"].is_object();

                if (is_forge_universal && has_artifact) {
                    auto& art = lib["downloads"]["artifact"];
                    std::string art_path = art.value("path", "");
                    std::string art_url  = art.value("url", "");

                    std::string prism_version;
                    size_t forge_pos = art_path.find("/forge/");
                    if (forge_pos != std::string::npos) {
                        size_t ver_start = forge_pos + 7;
                        size_t ver_end = art_path.find('/', ver_start);
                        if (ver_end != std::string::npos)
                            prism_version = art_path.substr(ver_start, ver_end - ver_start);
                    }
                    if (!prism_version.empty())
                        lib["name"] = "net.minecraftforge:forge:" + prism_version + ":universal";

                    force_universal_suffix(art_path);

                    if (art_url.empty()
                        || art_url.rfind("https://minecraft.net", 0) == 0
                        || art_url.rfind("http://minecraft.net", 0) == 0) {
                        art_url = "https://maven.minecraftforge.net/" + art_path;
                    } else {
                        force_universal_suffix(art_url);
                    }

                    art["path"] = art_path;
                    art["url"]  = art_url;
                    art["sha1"] = "";

                    continue;
                }

                if (has_artifact) continue;

                std::vector<std::string> parts;
                size_t start = 0, end = 0;
                while ((end = name.find(':', start)) != std::string::npos) {
                    parts.push_back(name.substr(start, end - start));
                    start = end + 1;
                }
                parts.push_back(name.substr(start));
                if (parts.size() < 3 || parts[0].empty() || parts[1].empty() || parts[2].empty())
                    continue;

                std::string group_path = parts[0];
                std::replace(group_path.begin(), group_path.end(), '.', '/');
                const std::string artifact   = parts[1];
                const std::string version    = parts[2];
                const std::string classifier = (parts.size() > 3) ? parts[3] : "";

                const std::string jar_name =
                    artifact + "-" + version +
                    (classifier.empty() ? "" : "-" + classifier) + ".jar";
                const std::string maven_path =
                    group_path + "/" + artifact + "/" + version + "/" + jar_name;

                std::string base = lib.value("url", std::string());
                if (base.empty()
                    || base.rfind("https://minecraft.net", 0) == 0
                    || base.rfind("http://minecraft.net", 0) == 0) {
                    static const char* mojang[] = {
                        "net.minecraft:launchwrapper:", "lzma:lzma:", "org.ow2.asm:",
                        "com.google.guava:", "org.apache.commons:"
                    };
                    bool is_mojang = false;
                    for (auto p : mojang)
                        if (name.rfind(p, 0) == 0) { is_mojang = true; break; }
                    base = is_mojang ? "https://libraries.minecraft.net/"
                                     : "https://maven.minecraftforge.net/";
                }
                if (base.back() != '/') base += '/';

                if (!lib.contains("downloads") || !lib["downloads"].is_object())
                    lib["downloads"] = json::object();
                if (!lib["downloads"].contains("artifact") || !lib["downloads"]["artifact"].is_object())
                    lib["downloads"]["artifact"] = json::object();

                lib["downloads"]["artifact"]["path"] = maven_path;
                lib["downloads"]["artifact"]["url"]  = base + maven_path;
                lib["downloads"]["artifact"].erase("sha1");
            }
        }

        if (!forge_metadata.contains("mainClass")) {
            forge_metadata["mainClass"] = "net.minecraft.launchwrapper.Launch";
        }

        const fs::path profile_json = installed_dir / (expected_id + ".json");
        {
            std::ofstream out(profile_json);
            if (!out) {
                set_install_error("Failed to write Forge profile JSON: " + profile_json.string());
                return "";
            }
            out << forge_metadata.dump(2);
        }

        report_progress(30, "Installing Forge...");
        LOG_INFO("Installing Forge " << forge_version << " for Minecraft " << mc_version);

        if (!ensure_libraries(installed_dir, forge_metadata, 30, 45)) {
            if (g_last_error.empty())
                set_install_error("Failed to download Forge libraries");
            std::error_code cleanup_ec;
            fs::remove(profile_json, cleanup_ec);
            return "";
        }

        mark_version_complete(installed_dir);
        report_progress(45, "Forge installed");
        LOG_INFO("Forge installed: " << expected_id);
        return expected_id;
    } catch (const std::exception& e) {
        set_install_error(std::string("Forge install error: ") + e.what());
        return "";
    }
}

namespace {
    bool is_safe_token(const std::string& s)
    {
        if (s.empty()) return false;
        for (char c : s) {
            if (!(std::isalnum((unsigned char)c) || c == '.' ||
                  c == '_' || c == '-' || c == '+'))
                return false;
        }
        return true;
    }

    std::string path_utf8(const fs::path& p)
    {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

    // Newest version in the list: releases win over pre-releases ("-beta", "-alpha"),
    // then the numeric parts are compared. Neither Quilt meta nor NeoForge maven
    // guarantee that the list is sorted.
    std::string pick_latest_version(const std::vector<std::string>& versions)
    {
        auto key = [](const std::string& v) {
            std::vector<long> nums{ v.find('-') == std::string::npos ? 1L : 0L };
            for (size_t i = 0; i < v.size();) {
                if (!std::isdigit((unsigned char)v[i])) { ++i; continue; }
                long n = 0;
                for (; i < v.size() && std::isdigit((unsigned char)v[i]); ++i)
                    n = n * 10 + (v[i] - '0');
                nums.push_back(n);
            }
            return nums;
        };
        std::string best;
        std::vector<long> best_key;
        for (const auto& v : versions) {
            auto k = key(v);
            if (best.empty() || k > best_key) { best = v; best_key = std::move(k); }
        }
        return best;
    }

    // Already installed (.complete) version whose id starts with `prefix` and ends with `suffix`.
    std::string find_installed_version(const std::string& prefix, const std::string& suffix)
    {
        const fs::path versions_dir = launcher_paths::shared_versions_dir();
        std::error_code ec;
        if (!fs::exists(versions_dir, ec)) return "";
        for (const auto& entry : fs::directory_iterator(versions_dir, ec)) {
            if (ec || !entry.is_directory()) continue;
            const std::string id = entry.path().filename().string();
            if (id.rfind(prefix, 0) != 0) continue;
            if (id.size() < prefix.size() + suffix.size() ||
                id.compare(id.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
            if (!version_complete(entry.path())) continue;
            if (!fs::is_regular_file(entry.path() / (id + ".json"))) continue;
            return id;
        }
        return "";
    }
}

std::string MinecraftInstaller::installQuilt(const std::string& mc_version)
{
    g_cancel_requested.store(false);
    reset_speed_state();
    g_last_error.clear();

    try {
        if (!is_safe_token(mc_version)) {
            set_install_error("Invalid Minecraft version for Quilt");
            return "";
        }

        std::string existing = find_installed_version("quilt-loader-", "-" + mc_version);
        if (!existing.empty()) return existing;

        const std::string meta_url =
            "https://meta.quiltmc.org/v3/versions/loader/" + mc_version;

        std::vector<std::string> loaders;
        const json loader_list = download_json(meta_url);
        if (loader_list.is_array()) {
            for (const auto& entry : loader_list) {
                if (!entry.is_object() || !entry.contains("loader")) continue;
                std::string v = entry["loader"].value("version", "");
                if (is_safe_token(v)) loaders.push_back(std::move(v));
            }
        }
        const std::string loader_version = pick_latest_version(loaders);
        if (loader_version.empty()) {
            set_install_error("No Quilt loader found for Minecraft " + mc_version);
            return "";
        }
        LOG_INFO("Using Quilt loader " << loader_version);

        if (!install(mc_version)) {
            if (g_last_error.empty())
                set_install_error("Failed to install Vanilla " + mc_version);
            return "";
        }

        json profile = download_json(meta_url + "/" + loader_version + "/profile/json");
        if (!profile.is_object() || !profile.contains("libraries") ||
            !profile["libraries"].is_array()) {
            set_install_error("Invalid Quilt profile for " + mc_version);
            return "";
        }

        const std::string expected_id =
            profile.value("id", "quilt-loader-" + loader_version + "-" + mc_version);
        if (!is_safe_token(expected_id) || expected_id.rfind("quilt-loader-", 0) != 0) {
            set_install_error("Unexpected Quilt profile id: " + expected_id);
            return "";
        }

        // Quilt libraries only carry "name" + "url" (maven base), while
        // ensure_libraries downloads what is listed in downloads.artifact.
        for (auto& lib : profile["libraries"]) {
            if (!lib.is_object() || !lib.contains("name") || !lib["name"].is_string()) continue;
            if (lib.contains("downloads") && lib["downloads"].contains("artifact")) continue;
            std::string rel;
            if (!maven_relative_path(lib["name"].get<std::string>(), rel)) continue;
            std::string base = lib.value("url", "https://maven.quiltmc.org/repository/release/");
            if (!base.empty() && base.back() != '/') base += '/';
            lib["downloads"]["artifact"]["path"] = rel;
            lib["downloads"]["artifact"]["url"]  = base + rel;
        }

        const fs::path installed_dir = launcher_paths::shared_versions_dir() / expected_id;
        fs::create_directories(installed_dir);

        const fs::path profile_json = installed_dir / (expected_id + ".json");
        {
            std::ofstream out(profile_json);
            if (!out) {
                set_install_error("Failed to write Quilt profile JSON: " + profile_json.string());
                return "";
            }
            out << profile.dump(2);
        }

        report_progress(30, "Installing Quilt...");
        LOG_INFO("Installing Quilt " << loader_version << " for Minecraft " << mc_version);

        if (!ensure_libraries(installed_dir, profile, 30, 45)) {
            if (g_last_error.empty())
                set_install_error("Failed to download Quilt libraries");
            std::error_code cleanup_ec;
            fs::remove(profile_json, cleanup_ec);
            return "";
        }

        mark_version_complete(installed_dir);
        report_progress(45, "Quilt installed");
        LOG_INFO("Quilt installed: " << expected_id);
        return expected_id;
    } catch (const std::exception& e) {
        set_install_error(std::string("Quilt install error: ") + e.what());
        return "";
    }
}

std::string MinecraftInstaller::installNeoForge(const std::string& mc_version)
{
    g_cancel_requested.store(false);
    reset_speed_state();
    g_last_error.clear();

    try {
        if (!is_safe_token(mc_version)) {
            set_install_error("Invalid Minecraft version for NeoForge");
            return "";
        }

        // NeoForge version = Minecraft version without the leading "1.":
        //   1.21.1 -> 21.1.x, 1.21 -> 21.0.x, 26.1 -> 26.1.0.x, 26.1.2 -> 26.1.2.x.
        // 1.20.1 is the exception: it is published as the legacy artifact
        // net.neoforged:forge (1.20.1-47.1.x) and installs as "1.20.1-forge-47.1.x".
        const bool legacy = (mc_version == "1.20.1");
        const std::string artifact = legacy ? "forge" : "neoforge";
        std::string version_prefix;
        if (legacy) {
            version_prefix = "1.20.1-47.1.";
        } else {
            version_prefix = mc_version.rfind("1.", 0) == 0 ? mc_version.substr(2) : mc_version;
            if (std::count(mc_version.begin(), mc_version.end(), '.') < 2)
                version_prefix += ".0";
            version_prefix += ".";
        }
        const std::string id_prefix =
            legacy ? "1.20.1-forge-47.1." : "neoforge-" + version_prefix;

        std::string existing = find_installed_version(id_prefix, "");
        if (!existing.empty()) return existing;

        const std::string maven = "https://maven.neoforged.net/";
        const json listing = download_json(
            maven + "api/maven/versions/releases/net/neoforged/" + artifact);

        std::vector<std::string> candidates;
        if (listing.contains("versions") && listing["versions"].is_array()) {
            for (const auto& v : listing["versions"]) {
                if (!v.is_string()) continue;
                std::string s = v.get<std::string>();
                if (s.rfind(version_prefix, 0) == 0 && is_safe_token(s))
                    candidates.push_back(std::move(s));
            }
        }
        const std::string neoforge_version = pick_latest_version(candidates);
        if (neoforge_version.empty()) {
            set_install_error("No NeoForge build found for Minecraft " + mc_version);
            return "";
        }
        const std::string expected_id =
            legacy ? "1.20.1-forge-" + neoforge_version.substr(std::string("1.20.1-").size())
                   : "neoforge-" + neoforge_version;
        LOG_INFO("Using NeoForge " << neoforge_version);

        if (!install(mc_version)) {
            if (g_last_error.empty())
                set_install_error("Failed to install Vanilla " + mc_version);
            return "";
        }

        const std::string installer_name =
            artifact + "-" + neoforge_version + "-installer.jar";
        const fs::path installer_path = launcher_paths::cache_dir() / installer_name;
        if (!fs::exists(installer_path)) {
            LOG_INFO("Downloading NeoForge Installer...");
            fs::create_directories(installer_path.parent_path());
            const std::string url = maven + "releases/net/neoforged/" + artifact + "/" +
                                    neoforge_version + "/" + installer_name;
            if (!download(url, installer_path)) {
                if (g_last_error.empty())
                    set_install_error("Failed to download NeoForge Installer");
                return "";
            }
        }

        // The installer refuses to run without a launcher profile in the target dir.
        const fs::path minecraft_dir = launcher_paths::shared_dir();
        const fs::path profiles_json = minecraft_dir / "launcher_profiles.json";
        if (!fs::exists(profiles_json)) {
            std::ofstream out(profiles_json);
            out << "{\"profiles\":{}}";
        }

        std::string java_cmd = "java";
        try {
            JavaManager jm;
            fs::path jp = jm.ensureJava(jm.requiredMajor(mc_version));
            if (!jp.empty()) java_cmd = path_utf8(jp);
        } catch (...) {}

        report_progress(35, "Installing NeoForge...");
        LOG_INFO("Installing NeoForge " << neoforge_version << " for Minecraft " << mc_version);
        const fs::path neoforge_log = launcher_paths::cache_dir() / "neoforge-installer.log";
        std::string cmd = "\"" + java_cmd + "\" -jar \"" + path_utf8(installer_path) +
                          "\" --installClient \"" + path_utf8(minecraft_dir) +
                          "\" > \"" + path_utf8(neoforge_log) + "\" 2>&1";
        int status = std::system(cmd.c_str());
        std::string neoforge_out;
        {
            std::ifstream in(neoforge_log);
            std::ostringstream ss;
            ss << in.rdbuf();
            neoforge_out = ss.str();
        }
        if (status != 0) {
            log_output_lines("NeoForge installer: ", neoforge_out);
            std::string line = pick_error_line(neoforge_out);
            if (line.empty())
                line = "NeoForge installation failed (status " + std::to_string(status) + ")";
            set_install_error(line);
            return "";
        }

        const fs::path installed_dir = launcher_paths::shared_versions_dir() / expected_id;
        if (!fs::is_regular_file(installed_dir / (expected_id + ".json"))) {
            log_output_lines("NeoForge installer: ", neoforge_out);
            set_install_error("NeoForge version not found after install: " + expected_id);
            return "";
        }

        mark_version_complete(installed_dir);
        report_progress(45, "NeoForge installed");
        LOG_INFO("NeoForge installed: " << expected_id);
        return expected_id;
    } catch (const std::exception& e) {
        set_install_error(std::string("NeoForge install error: ") + e.what());
        return "";
    }
}

const std::unordered_map<std::string, std::string> FabricVersionMap::COMPATIBILITY = {

    {"1.19.4", "0.15.11"},
    {"1.20.1", "0.15.11"},
    {"1.20.4", "0.16.5"},
    {"1.20.6", "0.16.14"},
    {"1.21.1", "0.16.14"},
    {"26.1", "0.20.1"},
    {"26.2", "0.20.1"},
    {"26.3", "0.20.1"},
};

std::string FabricVersionMap::getLoaderForVersion(const std::string& mc_version)
{

    try {
        json arr = download_json(
            "https://meta.fabricmc.net/v2/versions/loader/" + mc_version + "?limit=1");
        if (arr.is_array() && !arr.empty() && arr[0].contains("loader"))
            return arr[0]["loader"]["version"].get<std::string>();
    } catch (...) {}


    auto it = COMPATIBILITY.find(mc_version);
    if (it != COMPATIBILITY.end()) return it->second;


    return getClosestVersion(mc_version);
}

std::string FabricVersionMap::getClosestVersion(const std::string& mc_version)
{
    if (mc_version.rfind("26.", 0) == 0) return "0.20.1";

    size_t p1 = mc_version.find('.');
    size_t p2 = (p1 == std::string::npos) ? std::string::npos : mc_version.find('.', p1 + 1);
    if (p1 != std::string::npos && p2 != std::string::npos) {
        std::string base = mc_version.substr(0, p2);
        for (const auto& [v, loader] : COMPATIBILITY) {
            if (v.rfind(base, 0) == 0) return loader;
        }
    }
    return "0.16.14";
}
