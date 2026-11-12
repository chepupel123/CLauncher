#pragma once
#include "MinecraftInstaller.h"
#include <vector>

namespace downloader {
    struct DownloadItem {
        std::string url;
        fs::path dest;
        std::string sha1;
        std::string label;
        size_t size = 0;
    };

    extern std::string g_stage_label;
    extern std::string g_last_error;
    void set_install_error(const std::string& msg);
    void report_progress(int pct, const char* stage);
    void reset_speed_state();
    bool is_version_less(const std::string& version, int target_major,
                         int target_minor, int target_patch);
    bool version_complete(const fs::path& dir);
    void mark_version_complete(const fs::path& dir);
    bool download(const std::string& url, const fs::path& path);
    json download_json(const std::string& url);
    bool sha1_of_file(const fs::path& path, std::string& out_hex);
    bool verify_sha1(const fs::path& path, const std::string& expected);
    size_t json_file_size(const json& j);
    bool parallel_download(std::vector<DownloadItem> items, const std::string& stage_label,
                           unsigned max_workers = 16, int pct_from = -1, int pct_to = -1);
    bool library_allowed(const json& lib);
    bool ensure_libraries(const fs::path& version_dir, const json& metadata,
                          int pct_from = -1, int pct_to = -1);
}
