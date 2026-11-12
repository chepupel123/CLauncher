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

using namespace downloader;

namespace MinecraftInstaller {
    std::atomic<double> g_download_speed_mbps{0.0};
    std::atomic<int> g_download_eta_seconds{-1};
    std::atomic<bool> g_download_checking{false};
}

namespace downloader {
std::string g_stage_label;
    MinecraftInstaller::ProgressFn g_progress_fn = nullptr;
    void* g_progress_user = nullptr;
    std::string g_last_error;

    void set_install_error(const std::string& msg)
    {
        g_last_error = msg;
        LOG_ERROR(msg);
    }

    std::atomic<size_t> g_bytes_done{0};
    std::atomic<int> g_verify_inflight{0};
    int g_stage_pct_from = -1;
    int g_stage_pct_to = -1;

    std::chrono::steady_clock::time_point g_stage_start_time = std::chrono::steady_clock::now();
    size_t g_stage_bytes_at_start = 0;

    constexpr double kSpeedWarmupSec = 3.0;
    constexpr double kMinEtaSpeedMbps = 0.02;
    constexpr int kMaxEtaSec = 86400;

    thread_local bool t_count_bytes = false;
    thread_local size_t t_attempt_bytes = 0;
    thread_local bool t_write_error = false;

    void report_progress(int pct, const char* stage) {
        if (!g_progress_fn) return;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        g_progress_fn(static_cast<size_t>(pct), 0, stage, g_progress_user);
    }

    void reset_speed_state() {
        g_stage_start_time = std::chrono::steady_clock::now();
        g_stage_bytes_at_start = 0;
        g_bytes_done.store(0, std::memory_order_relaxed);
        g_verify_inflight.store(0, std::memory_order_relaxed);
        g_stage_pct_from = -1;
        g_stage_pct_to = -1;
        MinecraftInstaller::g_download_speed_mbps.store(0.0, std::memory_order_relaxed);
        MinecraftInstaller::g_download_eta_seconds.store(-1, std::memory_order_relaxed);
        MinecraftInstaller::g_download_checking.store(false, std::memory_order_relaxed);
    }

    void update_speed_eta(size_t bytes_done, size_t bytes_total) {
        if (bytes_total == 0) return;
        if (bytes_done > bytes_total) bytes_done = bytes_total;

        bool hashing = g_verify_inflight.load(std::memory_order_relaxed) > 0;

        if (hashing) {
            MinecraftInstaller::g_download_checking.store(true, std::memory_order_relaxed);
            return;
        }
        MinecraftInstaller::g_download_checking.store(false, std::memory_order_relaxed);

        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_stage_start_time).count();

        if (elapsed < kSpeedWarmupSec) {
            MinecraftInstaller::g_download_speed_mbps.store(0.0, std::memory_order_relaxed);
            MinecraftInstaller::g_download_eta_seconds.store(-1, std::memory_order_relaxed);
            return;
        }

        size_t transferred = (bytes_done >= g_stage_bytes_at_start)
            ? (bytes_done - g_stage_bytes_at_start) : 0;
        double speed_mbps = (static_cast<double>(transferred) / 1024.0 / 1024.0) / elapsed;
        MinecraftInstaller::g_download_speed_mbps.store(speed_mbps, std::memory_order_relaxed);

        size_t remaining = bytes_total - bytes_done;

        if (remaining < 1024 * 1024) {
            MinecraftInstaller::g_download_eta_seconds.store(1, std::memory_order_relaxed);
            return;
        }

        if (speed_mbps < kMinEtaSpeedMbps) {
            MinecraftInstaller::g_download_eta_seconds.store(-1, std::memory_order_relaxed);
            return;
        }

        double speed_bps = speed_mbps * 1024.0 * 1024.0;
        double eta_d = static_cast<double>(remaining) / speed_bps;
        int eta = -1;
        if (eta_d > 0.0 && eta_d <= static_cast<double>(kMaxEtaSec))
            eta = static_cast<int>(std::ceil(eta_d));
        MinecraftInstaller::g_download_eta_seconds.store(eta, std::memory_order_relaxed);
    }
    bool is_version_less(const std::string& version, int target_major,
                                int target_minor, int target_patch)
    {
        int major = 0, minor = 0, patch = 0;
        std::sscanf(version.c_str(), "%d.%d.%d", &major, &minor, &patch);
        if (major != target_major) return major < target_major;
        if (minor != target_minor) return minor < target_minor;
        return patch < target_patch;
    }

    bool version_complete(const fs::path& dir)
    {
        std::error_code ec;
        return fs::is_regular_file(dir / ".complete", ec);
    }

    void mark_version_complete(const fs::path& dir)
    {
        std::ofstream(dir / ".complete", std::ios::binary).put('\n');
    }

    std::once_flag curl_global_once;
    void ensure_curl_global()
    {
        std::call_once(curl_global_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    bool apply_tls(CURL* curl)
    {
        ensure_curl_global();
        return curl_tls::apply(curl);
    }

    std::string log_curl_failure(const std::string& url, CURLcode rc, int attempt = 0)
    {
        std::ostringstream ss;
        ss << "Download failed";
        if (attempt) ss << " (attempt " << attempt << ")";
        ss << " [" << (g_stage_label.empty() ? "down" : g_stage_label.c_str()) << "] "
           << curl_easy_strerror(rc) << ": " << url;
        std::string msg = ss.str();
        LOG_ERROR(msg);
        return msg;
    }

    size_t write_file(void* ptr, size_t size, size_t count, FILE* file)
    {
        size_t n = fwrite(ptr, size, count, file);
        if (n != count) t_write_error = true;
        size_t nbytes = n * size;
        if (t_count_bytes && nbytes > 0) {
            t_attempt_bytes += nbytes;
            g_bytes_done.fetch_add(nbytes, std::memory_order_relaxed);
        }
        return nbytes;
    }

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
        std::string last_fail;
        for (int attempt = 1; attempt <= 2; ++attempt) {
            CURL* curl = curl_easy_init();
            if (!curl) {
                set_install_error("Failed to initialize curl");
                return false;
            }
            apply_tls(curl);
            FILE* file = open_write_binary(path);
            if (!file) {
                curl_easy_cleanup(curl);
                set_install_error("Failed to open " + path.string());
                return false;
            }
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "CLauncher/1.0.3");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
            t_write_error = false;
            CURLcode result = curl_easy_perform(curl);
            if (t_write_error) result = CURLE_WRITE_ERROR;
            t_write_error = false;
            fclose(file);
            curl_easy_cleanup(curl);
            if (result == CURLE_OK) return true;
            fs::remove(path);
            last_fail = log_curl_failure(url, result, attempt);
        }
        if (last_fail.empty()) last_fail = "Download failed: " + url;
        g_last_error = last_fail;
        return false;
    }

    static size_t write_string_callback(void* ptr, size_t size, size_t nmemb, void* userdata) {
        std::string* str = static_cast<std::string*>(userdata);
        str->append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    }

    json download_json(const std::string& url)
    {
        ensure_curl_global();
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("Failed to initialize curl");
        apply_tls(curl);
        std::string data;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "CLauncher/1.0.3");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        CURLcode result = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK) {
            LOG_ERROR("Failed to download JSON: " << curl_easy_strerror(result)
                      << " (HTTP code: " << http_code << ") URL: " << url);
            throw std::runtime_error(
                std::string("Failed to download JSON: ") +
                curl_easy_strerror(result) +
                " (HTTP code: " + std::to_string(http_code) + ")"
            );
        }
        if (data.empty()) throw std::runtime_error("Downloaded JSON is empty");
        return json::parse(data);
    }


    bool sha1_of_file(const fs::path& path, std::string& out_hex)
    {
#ifdef _WIN32
        FILE* file = _wfopen(path.c_str(), L"rb");
#else
        FILE* file = fopen(path.c_str(), "rb");
#endif
        if (!file) return false;

        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
            EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha1(), nullptr) != 1) {
            fclose(file);
            return false;
        }

        unsigned char buf[8192];
        size_t nread = 0;
        while ((nread = fread(buf, 1, sizeof(buf), file)) > 0) {
            if (EVP_DigestUpdate(ctx.get(), buf, nread) != 1) {
                fclose(file);
                return false;
            }
        }
        if (ferror(file)) {
            fclose(file);
            return false;
        }
        fclose(file);

        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int md_len = 0;
        if (EVP_DigestFinal_ex(ctx.get(), md, &md_len) != 1) return false;

        static constexpr char hexdigits[] = "0123456789abcdef";
        out_hex.resize(static_cast<size_t>(md_len) * 2);
        for (unsigned int i = 0; i < md_len; ++i) {
            out_hex[i * 2]     = hexdigits[(md[i] >> 4) & 0x0F];
            out_hex[i * 2 + 1] = hexdigits[md[i] & 0x0F];
        }
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




    size_t json_file_size(const json& j)
    {
        if (!j.contains("size") || !j["size"].is_number_integer()) return 0;
        try {
            int64_t sz = j["size"].get<int64_t>();
            if (sz <= 0) return 0;
            return static_cast<size_t>(sz);
        } catch (...) {
            return 0;
        }
    }



    bool download_item(CURL* curl, const DownloadItem& it)
    {
        t_count_bytes = false;
        t_attempt_bytes = 0;
        try {
            fs::create_directories(it.dest.parent_path());
            fs::path tmp = it.dest;
            tmp += ".part";
            FILE* file = open_write_binary(tmp);
            if (!file) return false;

            curl_easy_reset(curl);
            apply_tls(curl);
            curl_easy_setopt(curl, CURLOPT_URL, it.url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "CLauncher/1.0.3");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 45L);

            t_count_bytes = it.size > 0;
            t_write_error = false;
            CURLcode result = curl_easy_perform(curl);
            if (t_write_error) result = CURLE_WRITE_ERROR;
            t_write_error = false;
            t_count_bytes = false;
            fclose(file);

            bool ok = (result == CURLE_OK);
            if (!ok) log_curl_failure(it.url, result);
            if (ok && !it.sha1.empty()) {
                g_verify_inflight.fetch_add(1, std::memory_order_relaxed);
                ok = verify_sha1(tmp, it.sha1);
                g_verify_inflight.fetch_sub(1, std::memory_order_relaxed);
                if (!ok) {
                    std::error_code ec;
                    LOG_WARN("SHA1 mismatch for " << it.dest.filename().string()
                        << " (size=" << fs::file_size(tmp, ec)
                        << ", expected=" << it.size
                        << ", url=" << it.url << ")");
                }
            }
            if (ok) {
#ifdef _WIN32
                ok = MoveFileExW(tmp.c_str(), it.dest.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
                std::error_code ec;
                fs::rename(tmp, it.dest, ec);
                ok = !ec;
#endif
            }
            if (!ok) {
                std::error_code ec;
                fs::remove(tmp, ec);
                if (t_attempt_bytes != 0) {
                    g_bytes_done.fetch_sub(t_attempt_bytes, std::memory_order_relaxed);
                    t_attempt_bytes = 0;
                }
            }
            return ok;
        } catch (...) {
            t_count_bytes = false;
            if (t_attempt_bytes != 0) {
                g_bytes_done.fetch_sub(t_attempt_bytes, std::memory_order_relaxed);
                t_attempt_bytes = 0;
            }
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
                           unsigned max_workers, int pct_from, int pct_to)
    {
        if (MinecraftInstaller::is_cancelled()) {
            set_install_error("Cancelled by user");
            return false;
        }
        if (items.empty()) return true;
        ensure_curl_global();


        std::vector<DownloadItem> todo;
        todo.reserve(items.size());
        for (auto& it : items) {
            if (MinecraftInstaller::is_cancelled()) {
                set_install_error("Cancelled by user");
                return false;
            }
            bool cached = fs::exists(it.dest) &&
                          (it.sha1.empty() ? true : verify_sha1(it.dest, it.sha1));
            if (!cached) todo.push_back(std::move(it));
        }
        if (todo.empty()) return true;

        const size_t total = todo.size();
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        unsigned by_cpu = hw * 4;
        unsigned by_files = 16;
        if (total <= 10) by_files = 4;
        else if (total <= 100) by_files = 8;
        unsigned workers = std::min(by_cpu, by_files);
        workers = std::min(workers, max_workers);


        if (const char* env = std::getenv("LAUNCHER_DL_THREADS")) {
            long n = std::strtol(env, nullptr, 10);
            if (n >= 1 && n <= 64) workers = static_cast<unsigned>(n);
        }
        if (workers > total) workers = static_cast<unsigned>(total);
        g_stage_label = stage_label;
        LOG_INFO(stage_label << ": " << total << " files to download, "
                 << workers << " threads");

        size_t bytes_total = 0;
        for (const auto& it : todo)
            if (it.size > 0) bytes_total += it.size;
        reset_speed_state();
        g_stage_pct_from = pct_from;
        g_stage_pct_to = pct_to;

        std::atomic<size_t> next(0), done(0);
        std::atomic<int> workers_alive(workers);
        std::mutex fail_mtx;
        std::vector<DownloadItem> failed;

        auto worker = [&] {
            CURL* curl = curl_easy_init();
            if (!curl) {
                workers_alive.fetch_sub(1);
                return;
            }
            for (;;) {
                if (MinecraftInstaller::is_cancelled()) break;
                size_t i = next.fetch_add(1);
                if (i >= total) break;
                const DownloadItem& it = todo[i];
                bool ok = download_item(curl, it);
                if (!ok && !MinecraftInstaller::is_cancelled())
                    ok = download_item(curl, it);
                if (!ok && !MinecraftInstaller::is_cancelled()) {
                    std::lock_guard<std::mutex> lk(fail_mtx);
                    failed.push_back(it);
                }
                done.fetch_add(1);
            }
            curl_easy_cleanup(curl);
            workers_alive.fetch_sub(1);
        };

        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (unsigned w = 0; w < workers; ++w) pool.emplace_back(worker);


        auto pct_now = [&](size_t files_done) -> int {
            double frac = 0.0;
            if (bytes_total > 0) {
                size_t bytes_done = g_bytes_done.load(std::memory_order_relaxed);
                if (bytes_done > bytes_total) bytes_done = bytes_total;
                frac = static_cast<double>(bytes_done) / static_cast<double>(bytes_total);
            } else if (total > 0) {
                frac = static_cast<double>(files_done) / static_cast<double>(total);
            }
            if (pct_from < 0 || pct_to <= pct_from)
                return static_cast<int>(100.0 * frac);
            return pct_from + static_cast<int>((pct_to - pct_from) * frac);
        };
        auto publish = [&](size_t d) {
            if (bytes_total == 0) {
                report_progress(pct_now(d), stage_label.c_str());
                return;
            }
            size_t bytes_done = g_bytes_done.load(std::memory_order_relaxed);
            if (bytes_done > bytes_total) bytes_done = bytes_total;
            update_speed_eta(bytes_done, bytes_total);
            if (!g_progress_fn) return;
            g_progress_fn(bytes_done, bytes_total, stage_label.c_str(), g_progress_user);
        };
        bool ok = true;
        while (done.load(std::memory_order_relaxed) < total) {
            if (MinecraftInstaller::is_cancelled()) {
                set_install_error("Cancelled by user");
                ok = false;
                break;
            }
            if (workers_alive.load() == 0 && done.load() < total) {
                set_install_error("Download failed: all workers exited");
                ok = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            size_t d = done.load(std::memory_order_relaxed);
            publish(d);
        }
        for (auto& t : pool) t.join();
        if (!MinecraftInstaller::is_cancelled()) publish(total);

        if (MinecraftInstaller::is_cancelled()) {
            set_install_error("Cancelled by user");
            ok = false;
        }
        if (!MinecraftInstaller::is_cancelled() && !failed.empty()) {
            LOG_WARN(stage_label << ": " << failed.size()
                     << " file(s) failed in pool, retrying one by one...");
            std::vector<DownloadItem> still_failed;
            for (const auto& it : failed) {
                if (MinecraftInstaller::is_cancelled()) {
                    set_install_error("Cancelled by user");
                    ok = false;
                    break;
                }
                if (!download_item_once(it)) still_failed.push_back(it);
                if (!MinecraftInstaller::is_cancelled()) publish(total);
            }
            if (!still_failed.empty()) {
                const auto& first = still_failed.front();
                std::string msg = "Failed to download: " + first.url;
                if (!first.label.empty()) msg += " (" + first.label + ")";
                if (still_failed.size() > 1)
                    msg += " and " + std::to_string(still_failed.size() - 1) + " more";
                g_last_error = msg;
                for (const auto& it : still_failed)
                    LOG_ERROR("Failed to download: " << it.url
                              << (it.label.empty() ? "" : " (" + it.label + ")"));
                ok = false;
            }
        }
        if (MinecraftInstaller::is_cancelled()) {
            set_install_error("Cancelled by user");
            ok = false;
        }
        reset_speed_state();
        return ok;
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
                          int pct_from, int pct_to)
    {
        if (!metadata.contains("libraries")) {
            set_install_error("Metadata has no libraries section");
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
            it.size = json_file_size(artifact);
            items.push_back(std::move(it));
        }
        return parallel_download(std::move(items), "Libraries", 16, pct_from, pct_to);
    }

}

void MinecraftInstaller::set_progress_callback(ProgressFn fn, void* user) {
    g_progress_fn = fn;
    g_progress_user = user;
}

int MinecraftInstaller::bar_percent(size_t bytes_done, size_t bytes_total)
{
    if (bytes_total == 0) return 0;
    if (bytes_done > bytes_total) bytes_done = bytes_total;
    double frac = static_cast<double>(bytes_done) / static_cast<double>(bytes_total);
    if (g_stage_pct_from >= 0 && g_stage_pct_to > g_stage_pct_from)
        return g_stage_pct_from + static_cast<int>((g_stage_pct_to - g_stage_pct_from) * frac);
    return static_cast<int>(100.0 * frac);
}

std::string MinecraftInstaller::last_error()
{
    return g_last_error;
}
