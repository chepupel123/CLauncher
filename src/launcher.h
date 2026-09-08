#pragma once
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>

enum class ModLoader { Vanilla, Fabric };

enum class UiLang { En = 0, Ru = 1 };

struct L10n {
    std::string title, language, nickname, version, mod_loader;
    std::string perf_mods, perf_mods_tip;
    std::string mods_need_fabric, mods_need_fabric_tip;
    std::string fabric26, fabric26_tip;
    std::string my_mods, my_mods_tip;
    std::string fabric_not_installed, fabric_unsupported;
    std::string resource_packs, rp_tip;
    std::string memory_alloc, selected_mb_fmt, play, discord;
    std::string ready, preparing, installing_vanilla, installing_fabric_for;
    std::string fabric_installed, install_failed, installing_perf_mods;
    std::string mods_failed, launching, game_launched, launch_failed;
    std::string font_warning;
    std::string invalid_nick_tip;
};

class Launcher {
public:
    struct Config {
        std::string nickname = "Steve";
        std::string selected_version;
        int memory_mb = 1024;
        std::string discord_url = "https://discord.gg/aQ7htke3mU";
        ModLoader mod_loader = ModLoader::Vanilla;
    };

    Launcher();
    ~Launcher();

    void init();
    void render();
    bool should_close() const;

    Config& get_config() { return config_; }
    const std::vector<std::string>& get_versions() const { return release_versions_; }



    void set_status(const std::string& text, float progress);


    UiLang ui_lang() const { return lang_; }

private:
    Config config_;
    std::vector<std::string> release_versions_;
    int memory_index_ = 1;
    std::atomic<bool> should_close_{false};


    std::atomic<bool> is_working{false};
    std::string status_text;
    float progress = 0.0f;
    std::mutex status_mutex_;

    std::thread worker_;


    UiLang lang_ = UiLang::En;
    const L10n& tr() const;


    void load_settings();
    void save_settings() const;


    bool perf_mods_checked_ = true;


    int invalid_nick_drops_ = 0;


    char nickname_buf_[32] = "Steve";

    struct GLFWwindow* window_ = nullptr;
    unsigned int clear_color_ = 0x1e1e2e;

    void setup_imgui();
    void draw_ui();
    void handle_play_button();
    void play_worker(Config cfg, bool install_mods, const L10n& L);
};
