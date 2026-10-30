#pragma once
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace MinecraftInstaller
{
    using ProgressFn = void (*)(size_t bytes_done, size_t bytes_total,
                                const char* stage, void* user);
    void set_progress_callback(ProgressFn fn, void* user);
    void request_cancel();
    bool is_cancelled();

    // Пишет поток загрузки, читает UI. ETA -1 = неизвестно.
    extern std::atomic<double> g_download_speed_mbps;
    extern std::atomic<int> g_download_eta_seconds;
    extern std::atomic<bool> g_download_checking;

    // Процент 0..100 внутри окна текущей стадии (libraries 10..45 и т.п.).
    int bar_percent(size_t bytes_done, size_t bytes_total);

    bool install(const std::string& version);
    std::string installFabric(const std::string& version);

    // Причина последнего сбоя install / installFabric. Пусто, если её не было.
    std::string last_error();

    bool installPerformanceMods(const std::filesystem::path& game_dir,
                                const std::string& mc_version);
    std::string findFabricDir(const std::string& mc_version);
}

class FabricVersionMap
{
public:
    static std::string getLoaderForVersion(const std::string& mc_version);

private:
    static const std::unordered_map<std::string, std::string> COMPATIBILITY;
    static std::string getClosestVersion(const std::string& mc_version);
};
