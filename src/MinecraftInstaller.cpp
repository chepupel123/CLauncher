#include "MinecraftInstaller.h"
#include "version_manager.h"
#include "JavaManager.h"
#include "paths.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
    MinecraftInstaller::ProgressFn g_progress_fn = nullptr;
    void* g_progress_user = nullptr;


    void report_progress(int pct, const char* stage) {
        if (g_progress_fn) g_progress_fn(pct, stage, g_progress_user);
    }
}

namespace
{


    std::once_flag curl_global_once;
    void ensure_curl_global()
    {
        std::call_once(curl_global_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    size_t write_file(void* ptr, size_t size, size_t count, FILE* file)
    {
        return fwrite(ptr, size, count, file);
    }

    // Кроссплатформенное открытие файла на запись: на Windows path::value_type
    // это wchar_t, поэтому fopen(path.c_str()) не скомпилируется — нужен _wfopen.
    static FILE* open_write_binary(const fs::path& path)
    {
#ifdef _WIN32
        return _wfopen(path.c_str(), L"wb");
#else
        return fopen(path.c_str(), "wb");
#endif
    }


    bool download(const std::string& url, const fs::path& path)
    {
        ensure_curl_global();
        fs::create_directories(path.parent_path());
        for (int attempt = 1; attempt <= 2; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) return false;
            FILE* file = open_write_binary(path);
            if (!file) { curl_easy_cleanup(curl); return false; }
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Minecraft-Launcher/1.0");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            CURLcode result = curl_easy_perform(curl);
            fclose(file);
            curl_easy_cleanup(curl);
            if (result == CURLE_OK) return true;
            fs::remove(path);
            std::cerr << "Download failed (attempt " << attempt << "/2): " << url << '\n';
        }
        return false;
    }

    static size_t write_string_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
        std::string* str = static_cast<std::string*>(userdata);
        str->append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    }

    static json download_json(const std::string& url)
    {
        ensure_curl_global();
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("Failed to initialize curl");
        std::string data;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Minecraft-Launcher/1.0");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        CURLcode result = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK) {
            throw std::runtime_error(
                std::string("Failed to download JSON: ") +
                curl_easy_strerror(result) +
                " (HTTP code: " + std::to_string(http_code) + ")"
            );
        }
        if (data.empty()) throw std::runtime_error("Downloaded JSON is empty");
        return json::parse(data);
    }


    class Sha1 {
    public:
        Sha1() { reset(); }
        void reset() {
            h_[0] = 0x67452301u; h_[1] = 0xEFCDAB89u; h_[2] = 0x98BADCFEu;
            h_[3] = 0x10325476u; h_[4] = 0xC3D2E1F0u;
            len_ = 0; buf_len_ = 0;
        }
        void update(const void* data, size_t n) {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            len_ += n;
            while (n > 0) {
                size_t take = std::min(n, size_t(64) - buf_len_);
                std::memcpy(buf_ + buf_len_, p, take);
                buf_len_ += take; p += take; n -= take;
                if (buf_len_ == 64) { process(buf_); buf_len_ = 0; }
            }
        }
        std::string hex() {
            uint64_t bits = len_ * 8;
            uint8_t pad = 0x80;
            update(&pad, 1);
            uint8_t z = 0;
            while (buf_len_ != 56) update(&z, 1);
            uint8_t lenb[8];
            for (int i = 0; i < 8; ++i) lenb[i] = uint8_t(bits >> (56 - 8 * i));
            update(lenb, 8);
            char out[41];
            for (int i = 0; i < 5; ++i) std::snprintf(out + i * 8, 9, "%08x", h_[i]);
            return std::string(out, 40);
        }
    private:
        void process(const uint8_t* p) {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
                       (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
            for (int i = 16; i < 80; ++i) {
                uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
                w[i] = (v << 1) | (v >> 31);
            }
            uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20)      { f = (b & c) | ((~b) & d);      k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d;                 k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else             { f = b ^ c ^ d;                 k = 0xCA62C1D6u; }
                uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];

                e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
            }
            h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e;
        }
        uint32_t h_[5];
        uint64_t len_;
        size_t buf_len_;
        uint8_t buf_[64];
    };

    bool sha1_of_file(const fs::path& path, std::string& out_hex)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        Sha1 h;
        char buf[8192];
        while (in) {
            in.read(buf, sizeof(buf));
            if (in.gcount() > 0) h.update(buf, static_cast<size_t>(in.gcount()));
        }
        out_hex = h.hex();
        return true;
    }

    std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    bool verify_sha1(const fs::path& path, const std::string& expected)
    {
        if (expected.empty()) return true;
        std::string actual;
        if (!sha1_of_file(path, actual)) return false;
        return actual == to_lower(expected);
    }



    struct DownloadItem {
        std::string url;
        fs::path dest;
        std::string sha1;
        std::string label;
    };



    bool download_item(CURL* curl, const DownloadItem& it)
    {
        try {
            fs::create_directories(it.dest.parent_path());
            fs::path tmp = it.dest.string() + ".part";
            FILE* file = open_write_binary(tmp);
            if (!file) return false;

            curl_easy_reset(curl);
            curl_easy_setopt(curl, CURLOPT_URL, it.url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Minecraft-Launcher/1.0");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);

            CURLcode result = curl_easy_perform(curl);
            fclose(file);

            bool ok = (result == CURLE_OK);
            if (ok && !it.sha1.empty()) ok = verify_sha1(tmp, it.sha1);
            if (ok) {
                std::error_code ec;
                fs::remove(it.dest, ec);
                fs::rename(tmp, it.dest, ec);
                ok = !ec;
            }
            if (!ok) { std::error_code ec; fs::remove(tmp, ec); }
            return ok;
        } catch (...) {
            return false;
        }
    }

    bool download_item_once(const DownloadItem& it)
    {
        ensure_curl_global();
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        bool ok = download_item(curl, it);
        curl_easy_cleanup(curl);
        return ok;
    }



    bool parallel_download(std::vector<DownloadItem> items, const std::string& stage_label,
                           unsigned max_workers = 16, int pct_from = -1, int pct_to = -1)
    {
        if (items.empty()) return true;
        ensure_curl_global();


        std::vector<DownloadItem> todo;
        todo.reserve(items.size());
        for (auto& it : items) {
            bool cached = fs::exists(it.dest) &&
                          (it.sha1.empty() ? true : verify_sha1(it.dest, it.sha1));
            if (!cached) todo.push_back(std::move(it));
        }
        if (todo.empty()) return true;

        const size_t total = todo.size();
        unsigned workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 4;
        workers = std::min(workers, max_workers);


        if (const char* env = std::getenv("LAUNCHER_DL_THREADS")) {
            long n = std::strtol(env, nullptr, 10);
            if (n >= 1 && n <= 64) workers = static_cast<unsigned>(n);
        }
        if (workers > total) workers = static_cast<unsigned>(total);

        std::cerr << stage_label << ": " << total
                  << " files to download, threads: " << workers << "\n";

        std::atomic<size_t> next(0), done(0);
        std::mutex fail_mtx;
        std::vector<DownloadItem> failed;

        auto worker = [&] {
            CURL* curl = curl_easy_init();
            if (!curl) return;
            for (;;) {
                size_t i = next.fetch_add(1);
                if (i >= total) break;
                const DownloadItem& it = todo[i];
                bool ok = download_item(curl, it);
                if (!ok) ok = download_item(curl, it);
                if (!ok) {
                    std::lock_guard<std::mutex> lk(fail_mtx);
                    failed.push_back(it);
                }
                done.fetch_add(1);
            }
            curl_easy_cleanup(curl);
        };

        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) pool.emplace_back(worker);


        auto pct_now = [&](size_t d) -> int {
            if (pct_from < 0 || pct_to <= pct_from)
                return static_cast<int>(100 * d / total);
            return pct_from + static_cast<int>((pct_to - pct_from) * d / total);
        };
        while (done.load(std::memory_order_relaxed) < total) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            size_t d = done.load(std::memory_order_relaxed);
            std::cout << "\r" << stage_label << ": " << d << "/" << total
                      << " (" << pct_now(d) << "%)      " << std::flush;
            report_progress(pct_now(d), stage_label.c_str());
        }
        std::cout << "\r" << stage_label << ": " << total << "/" << total << " (100%)\n";
        for (auto& t : pool) t.join();


        if (!failed.empty()) {
            std::cerr << stage_label << ": " << failed.size()
                      << " file(s) failed in pool, retrying one by one...\n";
            std::vector<DownloadItem> still_failed;
            for (const auto& it : failed) {
                if (!download_item_once(it)) still_failed.push_back(it);
            }
            if (!still_failed.empty()) {
                for (const auto& it : still_failed)
                    std::cerr << "Failed to download: " << it.url
                              << (it.label.empty() ? "" : " (" + it.label + ")") << '\n';
                return false;
            }
        }
        return true;
    }




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


    bool library_allowed(const json& lib)
    {
        if (!lib.contains("rules") || !lib["rules"].is_array()) return true;
        bool allowed = false;
        for (const auto& rule : lib["rules"]) {
            if (!rule.contains("action")) continue;
            bool os_match = true;
            if (rule.contains("os") && rule["os"].contains("name")) {
#if defined(_WIN32)
                os_match = (rule["os"]["name"].get<std::string>() == "windows");
#elif defined(__APPLE__)
                os_match = (rule["os"]["name"].get<std::string>() == "osx");
#else
                os_match = (rule["os"]["name"].get<std::string>() == "linux");
#endif
            }
            if (os_match) allowed = (rule["action"].get<std::string>() == "allow");
        }
        return allowed;
    }



    bool ensure_libraries(const fs::path& version_dir, const json& metadata,
                          int pct_from = -1, int pct_to = -1)
    {
        if (!metadata.contains("libraries")) {
            std::cerr << "Metadata has no libraries section\n";
            return false;
        }
        std::vector<DownloadItem> items;
        for (const auto& library : metadata["libraries"]) {
            if (!library_allowed(library)) continue;
            if (!library.contains("downloads") || !library["downloads"].contains("artifact")) continue;
            auto artifact = library["downloads"]["artifact"];
            std::string url = artifact.value("url", std::string());
            if (url.empty()) continue;
            DownloadItem it;
            it.url = url;
            it.dest = version_dir / "libraries" / artifact["path"].get<std::string>();
            it.sha1 = artifact.value("sha1", std::string());
            it.label = artifact["path"].get<std::string>();
            items.push_back(std::move(it));
        }
        return parallel_download(std::move(items), "Libraries", 16, pct_from, pct_to);
    }

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
            std::cout << "Downloading asset index...\n";
            if (!download(asset_index_url, index_file)) return false;
            if (!index_sha.empty() && !verify_sha1(index_file, index_sha)) {
                std::cerr << "SHA1 mismatch: asset index " << asset_index_id << '\n';
                return false;
            }
        }

        json asset_index;
        try {
            std::ifstream file(index_file);
            if (!file) { std::cerr << "Failed to open asset index\n"; return false; }
            file >> asset_index;
        } catch (const std::exception& e) {
            std::cerr << "Failed to parse asset index: " << e.what() << '\n';
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
        bool downloaded_any = false;

        for (const auto& library : metadata["libraries"]) {
            if (!library_allowed(library)) continue;
            if (!library.contains("downloads")) continue;
            const auto& downloads = library["downloads"];
            if (!downloads.contains("classifiers") ||
                !downloads["classifiers"].contains(native_classifier)) continue;
            auto native_info = downloads["classifiers"][native_classifier];
            std::string url = native_info["url"];
            std::string path = native_info["path"];
            fs::path dest_jar = version_dir / "libraries" / path;
            if (!fs::exists(dest_jar)) {
                std::cout << "Downloading natives: " << path << "\n";
                if (!download(url, dest_jar)) return false;
                downloaded_any = true;
            }
            native_jars.push_back(dest_jar);
        }



        bool natives_empty = fs::is_empty(natives_dir);
        if (!downloaded_any && !natives_empty) return true;

        for (const auto& jar : native_jars) {
#ifdef _WIN32
            // bsdtar (встроен в Windows 10/11) умеет распаковывать zip/jar,
            // в отличие от Expand-Archive, который требует расширение .zip.
            std::string cmd = "tar -xf \"" + jar.string() + "\" -C \"" + natives_dir.string() + "\"";
#else
            std::string cmd = "unzip -o -q \"" + jar.string() + "\" -d \"" + natives_dir.string() + "\"";
#endif
            if (std::system(cmd.c_str()) != 0) {
#ifdef _WIN32
                std::cerr << "Failed to extract natives (tar): " << jar << "\n";
#else
                std::cerr << "Failed to extract natives (is unzip installed?): "
                          << jar << "\n";
#endif
                return false;
            }
        }
        return true;
    }

}

void MinecraftInstaller::set_progress_callback(ProgressFn fn, void* user) {
    g_progress_fn = fn;
    g_progress_user = user;
}

bool MinecraftInstaller::install(const std::string& version)
{
    try
    {
        fs::path minecraft = launcher_paths::minecraft_dir();
        fs::path version_dir = minecraft / "versions" / version;
        fs::path version_json = version_dir / (version + ".json");
        fs::path client_jar = version_dir / (version + ".jar");

        bool fresh = false;
        json metadata;

        if (fs::exists(version_json) && fs::exists(client_jar)) {


            std::cout << "Minecraft " << version << " is already downloaded, verifying...\n";
            try {
                std::ifstream file(version_json);
                if (!file) throw std::runtime_error("cannot open");
                file >> metadata;
            } catch (const std::exception& e) {
                std::cerr << "version.json corrupted (" << e.what() << "), re-downloading\n";
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
                std::cerr << "Version not found in manifest: " << version << '\n';
                return false;
            }

            report_progress(2, "Version metadata...");
            std::cout << "Downloading Minecraft " << version << " metadata...\n";
            fs::create_directories(version_dir);
            metadata = download_json(version_url);
            {
                std::ofstream file(version_json);
                if (!file) { std::cerr << "Failed to write " << version_json << '\n'; return false; }
                file << metadata.dump(2);
            }

            if (!metadata.contains("downloads") || !metadata["downloads"].contains("client")) {
                std::cerr << "Client download information is missing\n";
                return false;
            }
            std::string client_url = metadata["downloads"]["client"]["url"];
            std::string client_sha = metadata["downloads"]["client"].value("sha1", std::string());
            report_progress(6, "Downloading client...");
            std::cout << "Downloading Minecraft client...\n";
            if (!download(client_url, client_jar)) return false;
            if (!client_sha.empty() && !verify_sha1(client_jar, client_sha)) {
                std::cerr << "Client SHA1 mismatch, retrying download...\n";
                if (!download(client_url, client_jar) || !verify_sha1(client_jar, client_sha)) {
                    std::cerr << "Client SHA1 mismatch persists\n";
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
                std::cerr << "Client JAR corrupted (SHA1 mismatch), re-downloading...\n";
                std::string client_url = metadata["downloads"]["client"]["url"];
                if (!download(client_url, client_jar) || !verify_sha1(client_jar, client_sha)) {
                    std::cerr << "Client JAR re-download failed\n";
                    return false;
                }
            }
        }
        if (!ensure_libraries(version_dir, metadata, 10, 45)) return false;
        if (!ensure_assets(minecraft, metadata, 45, 92)) return false;
        report_progress(92, "Natives...");
        if (!ensure_natives(version_dir, metadata)) return false;
        report_progress(98, "Done");

        std::cout << "Minecraft " << version << " installed successfully\n";
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "MinecraftInstaller error: " << e.what() << '\n';
        return false;
    }
}

bool MinecraftInstaller::installPerformanceMods(const std::string& fabric_version_name,
                                                const std::string& mc_version)
{
    try {
        fs::path mods_dir = launcher_paths::minecraft_dir() / "versions" / fabric_version_name / "mods";
        fs::create_directories(mods_dir);


        fs::path manifest_path = mods_dir / ".launcher-mods.json";
        json manifest = json::object();
        if (fs::exists(manifest_path)) {
            try {
                std::ifstream mf(manifest_path);
                mf >> manifest;
            } catch (...) {
                std::cerr << "WARNING: mods manifest corrupted, starting fresh\n";
                manifest = json::object();
            }
        }

        const std::pair<std::string, std::string> mods[] = {
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
                std::cerr << "Modrinth: no build of " << title << " for Minecraft "
                          << mc_version << "\n";
                return false;
            }

            const json& v = versions[0];
            const json* file = nullptr;
            if (v.contains("files") && v["files"].is_array()) {
                for (const auto& f : v["files"]) {
                    if (f.value("primary", false)) { file = &f; break; }
                }
                if (!file && !v["files"].empty()) file = &v["files"][0];
            }
            if (!file || !file->contains("url")) {
                std::cerr << "Modrinth: malformed response for " << title << "\n";
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
                if (!ec) std::cout << "Removed old version of " << title << ": "
                                   << old_file << "\n";
            }
            new_manifest[slug] = it.dest.filename().string();
            items.push_back(std::move(it));
        }

        if (!parallel_download(std::move(items), "Mods", 16, 45, 95)) return false;

        std::ofstream mf(manifest_path);
        mf << new_manifest.dump(2);
        std::cout << "✓ Performance mods installed: Sodium, Lithium, FerriteCore\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Mods install error: " << e.what() << '\n';
        return false;
    }
}

std::string MinecraftInstaller::findFabricDir(const std::string& mc_version)
{
    fs::path versions_dir = launcher_paths::minecraft_dir() / "versions";
    std::error_code ec;
    if (!fs::exists(versions_dir)) return "";

    const std::string suffix = "-" + mc_version;
    std::string best;
    fs::file_time_type best_time{};

    for (const auto& entry : fs::directory_iterator(versions_dir)) {
        if (!entry.is_directory()) continue;
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
    try {
        fs::path minecraft_dir = launcher_paths::minecraft_dir();


        if (!install(version)) {
            std::cerr << "Failed to install Vanilla " << version << "\n";
            return "";
        }


        fs::path profiles_path = minecraft_dir / "launcher_profiles.json";
        if (!fs::exists(profiles_path)) {
            std::cout << "Creating launcher_profiles.json...\n";
            std::ofstream profiles(profiles_path);
            if (!profiles.is_open()) { std::cerr << "Failed to create launcher_profiles.json\n"; return ""; }
            profiles << "{\n"
                     << "  \"profiles\": {},\n"
                     << "  \"settings\": {},\n"
                     << "  \"selectedProfile\": \"\",\n"
                     << "  \"clientToken\": \"\",\n"
                     << "  \"authenticationDatabase\": {}\n"
                     << "}\n";
        }


        fs::path launcher_dir = launcher_paths::launcher_dir();
        fs::path installer_path = launcher_dir / "fabric-installer.jar";
        if (!fs::exists(installer_path)) {
            std::cout << "Downloading Fabric Installer...\n";
            fs::create_directories(launcher_dir);
            std::string url = "https://maven.fabricmc.net/net/fabricmc/fabric-installer/1.0.1/fabric-installer-1.0.1.jar";
            if (!download(url, installer_path)) { std::cerr << "Failed to download Fabric Installer\n"; return ""; }
        }


        std::string loader_version = FabricVersionMap::getLoaderForVersion(version);
        std::cout << "Using Fabric loader " << loader_version << "\n";


        std::string java_cmd = "java";
        try {
            JavaManager jm;
            fs::path jp = jm.ensureJava(version);
            if (!jp.empty()) java_cmd = jp.string();
        } catch (...) {}

        report_progress(35, "Installing Fabric...");
        std::cout << "Installing Fabric...\n";
        std::string cmd = "\"" + java_cmd + "\" -jar \"" + installer_path.string() +
                          "\" client -dir \"" + minecraft_dir.string() +
                          "\" -mcversion " + version +
                          " -loader " + loader_version;
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "Fabric installation failed\n";
            return "";
        }


        std::string found_folder = findFabricDir(version);
        if (found_folder.empty()) { std::cerr << "Fabric directory not found\n"; return ""; }
        fs::path fabric_dir = minecraft_dir / "versions" / found_folder;




        fs::path target_jar = fabric_dir / (found_folder + ".jar");
        if (fs::exists(target_jar)) {
            std::cerr << "WARNING: found phantom " << target_jar << "\n"
                      << "  (this file must NOT exist — it caused\n"
                      << "  IllegalStateException: trying to load FabricLoaderImpl...)\n"
                      << "  Removing.\n";
            std::error_code ec;
            fs::remove(target_jar, ec);
        }


        fs::path profile_json = fabric_dir / (found_folder + ".json");
        if (!fs::exists(profile_json)) {
            std::cerr << "Fabric profile JSON not found: " << profile_json << "\n";
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
                    std::cout << "Downloading Fabric library: " << rel << '\n';
                    if (!download(base + rel, dest)) return "";
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "WARNING: failed to verify fabric libraries: " << e.what() << "\n";
        }

        report_progress(40, "Fabric installed");
        std::cout << "✓ Fabric installed: " << found_folder << "\n";
        return found_folder;
    } catch (const std::exception& e) {
        std::cerr << "Fabric install error: " << e.what() << '\n';
        return "";
    }
}

const std::unordered_map<std::string, std::string> FabricVersionMap::COMPATIBILITY = {

    {"1.19.4", "0.15.11"},
    {"1.20.1", "0.15.11"},
    {"1.20.4", "0.16.5"},
    {"1.20.6", "0.16.14"},
    {"1.21.1", "0.16.14"},
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
