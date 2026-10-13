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

namespace fs = std::filesystem;
static std::string g_stage_label;  // текущий этап загрузки — для логов
using json = nlohmann::json;

namespace MinecraftInstaller {
    std::atomic<double> g_download_speed_mbps{0.0};
    std::atomic<int> g_download_eta_seconds{-1};
    std::atomic<bool> g_download_checking{false};
    std::atomic<bool> g_cancel_requested{false};

    void request_cancel() { g_cancel_requested.store(true); }
    bool is_cancelled() { return g_cancel_requested.load(); }
}

namespace {
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

    constexpr double kSpeedWarmupSec = 10.0;
    constexpr double kMinEtaSpeedMbps = 0.1;
    constexpr int kMaxEtaSec = 86400;

    thread_local bool t_count_bytes = false;
    thread_local size_t t_attempt_bytes = 0;
    thread_local bool t_write_error = false;

    void report_progress(int pct, const char* stage) {
        if (!g_progress_fn) return;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        // bytes_total == 0: это не байты, а старый процент 0..100 в bytes_done.
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

    // Средняя скорость с начала этапа: (bytes_done - start) / elapsed.
    // Первые 10 с не публикуем. ETA скрыт при скорости < 0.1 МБ/с или > 24 ч.
    void update_speed_eta(size_t bytes_done, size_t bytes_total) {
        if (bytes_total == 0) return;
        if (bytes_done > bytes_total) bytes_done = bytes_total;

        bool hashing = g_verify_inflight.load(std::memory_order_relaxed) > 0;
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - g_stage_start_time).count();

        if (elapsed < kSpeedWarmupSec) {
            MinecraftInstaller::g_download_speed_mbps.store(0.0, std::memory_order_relaxed);
            MinecraftInstaller::g_download_eta_seconds.store(-1, std::memory_order_relaxed);
            MinecraftInstaller::g_download_checking.store(hashing, std::memory_order_relaxed);
            return;
        }

        size_t transferred = (bytes_done >= g_stage_bytes_at_start)
            ? (bytes_done - g_stage_bytes_at_start) : 0;
        double speed_mbps = (static_cast<double>(transferred) / 1024.0 / 1024.0) / elapsed;
        MinecraftInstaller::g_download_speed_mbps.store(speed_mbps, std::memory_order_relaxed);

        if (hashing && speed_mbps < kMinEtaSpeedMbps) {
            MinecraftInstaller::g_download_checking.store(true, std::memory_order_relaxed);
            MinecraftInstaller::g_download_speed_mbps.store(0.0, std::memory_order_relaxed);
            MinecraftInstaller::g_download_eta_seconds.store(-1, std::memory_order_relaxed);
            return;
        }
        MinecraftInstaller::g_download_checking.store(false, std::memory_order_relaxed);

        if (speed_mbps < kMinEtaSpeedMbps || bytes_done >= bytes_total) {
            MinecraftInstaller::g_download_eta_seconds.store(-1, std::memory_order_relaxed);
            return;
        }

        size_t remaining = bytes_total - bytes_done;
        double speed_bps = speed_mbps * 1024.0 * 1024.0;
        double eta_d = static_cast<double>(remaining) / speed_bps;
        int eta = -1;
        if (eta_d > 0.0 && eta_d <= static_cast<double>(kMaxEtaSec))
            eta = static_cast<int>(std::ceil(eta_d));
        MinecraftInstaller::g_download_eta_seconds.store(eta, std::memory_order_relaxed);
    }
}

namespace
{


    std::once_flag curl_global_once;
    void ensure_curl_global()
    {
        std::call_once(curl_global_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    // Применяем единую TLS-конфигурацию (NATIVE_CA / cacert.pem / системные CA)
    // к каждому curl-хэндлу. Проверка сертификатов НЕ отключается.
    bool apply_tls(CURL* curl)
    {
        ensure_curl_global();
        return curl_tls::apply(curl);
    }

    // Логируем сетевые ошибки в clauncher.log (уровень ERROR).
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
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Minecraft-Launcher/1.0");
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

    static json download_json(const std::string& url)
    {
        ensure_curl_global();
        CURL* curl = curl_easy_init();
        if (!curl) throw std::runtime_error("Failed to initialize curl");
        apply_tls(curl);
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



    struct DownloadItem {
        std::string url;
        fs::path dest;
        std::string sha1;
        std::string label;
        size_t size = 0;
    };

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
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Minecraft-Launcher/1.0");
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
                           unsigned max_workers = 16, int pct_from = -1, int pct_to = -1)
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
        unsigned workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 4;
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
            // bsdtar (встроен в Windows 10/11) умеет распаковывать zip/jar,
            // в отличие от Expand-Archive, который требует расширение .zip.
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

bool MinecraftInstaller::install(const std::string& version)
{
    g_cancel_requested.store(false);
    reset_speed_state();
    g_last_error.clear();
    try
    {
        fs::path minecraft = launcher_paths::minecraft_dir();
        fs::path version_dir = minecraft / "versions" / version;
        fs::path version_json = version_dir / (version + ".json");
        fs::path client_jar = version_dir / (version + ".jar");

        bool fresh = false;
        json metadata;

        if (fs::exists(version_json) && fs::exists(client_jar)) {


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

        LOG_INFO("Minecraft " << version << " installed successfully");
        return true;
    }
    catch (const std::exception& e) {
        set_install_error(std::string("MinecraftInstaller error: ") + e.what());
        return false;
    }
}

bool MinecraftInstaller::installPerformanceMods(const std::string&,
                                                const std::string& mc_version)
{
    g_cancel_requested.store(false);
    reset_speed_state();
    try {
        fs::path mods_dir = launcher_paths::minecraft_dir() / "mods";
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
        LOG_INFO("Performance mods installed: Sodium, Lithium, FerriteCore");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Mods install error: " << e.what());
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
    g_cancel_requested.store(false);
    fs::path fabric_temp_dir;
    try {
        fs::path minecraft_dir = launcher_paths::minecraft_dir();
        std::string existing_fabric_dir = findFabricDir(version);
        if (!existing_fabric_dir.empty()) return existing_fabric_dir;

        if (!install(version)) {
            if (g_last_error.empty())
                set_install_error("Failed to install Vanilla " + version);
            return "";
        }


        fs::path launcher_dir = launcher_paths::launcher_dir();
        fs::path installer_path = launcher_dir / "fabric-installer.jar";
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
        std::error_code replace_ec;
        fs::rename(fabric_dir, installed_fabric_dir, replace_ec);
        if (replace_ec)
            throw std::runtime_error("Failed to install Fabric version: " + replace_ec.message());
        std::error_code cleanup_ec;
        fs::remove_all(fabric_temp_dir, cleanup_ec);

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
