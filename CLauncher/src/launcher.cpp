#include "launcher.h"
#include "version_manager.h"
#include "java_launcher.h"
#include "MinecraftInstaller.h"
#include "paths.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;
using json = nlohmann::json;

static const int RAM_OPTIONS[] = {512, 1024, 2048, 3072, 4096};
static const int RAM_OPTIONS_COUNT = 5;

static int mcMajor(const std::string& id) {
    long major = 0;
    size_t pos = 0;
    while (pos < id.size() && std::isdigit(static_cast<unsigned char>(id[pos])))
        major = major * 10 + (id[pos++] - '0');
    return (pos == 0) ? 0 : static_cast<int>(major);
}

static bool nickname_char_ok(unsigned int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static std::string sanitize_nickname(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char b = static_cast<unsigned char>(in[i]);
        size_t len = (b < 0x80) ? 1 : (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;
        if (b < 0x80 && nickname_char_ok(b)) out += in[i];
        i += len;
    }
    return out;
}

static std::string sfmt(const std::string& tmpl, const std::string& arg) {
    auto p = tmpl.find("%s");
    if (p == std::string::npos) return tmpl;
    return tmpl.substr(0, p) + arg + tmpl.substr(p + 2);
}

static const L10n& l10n_en() {
    static const L10n L{
                "CLauncher",
             "Language",
             "Nickname",
              "Version",
           "Mod Loader:",
            "Performance mods: Sodium + Lithium + FerriteCore",
        "Sodium — render FPS, Lithium — game logic/ticks,\n"
                         "FerriteCore — memory usage.\n"
                         "Versions are picked automatically for the selected Minecraft version.",
             "Mods require the Fabric loader (select it above)",
         "Vanilla Minecraft cannot load mods.\n"
                                 "Switch Mod Loader to Fabric to enable mods.",
             "Fabric is not supported for 26.x (Vanilla works)",
         "Mojang changed the client architecture in 26.x\n"
                         "and Fabric support was intentionally cut for this line.\n"
                         "Use a 1.21.x or older version for mods.",
              "My Mods",
          "Mods folder for this Fabric version.\n"
                         "Drop your .jar files here — they load on launch.\n"
                         "Each Minecraft version has its own folder — no mod conflicts.",
         "Fabric for %s is not installed yet — press Play first",
           "Fabric is not supported for %s — use 1.21.x or older for mods",
         "Resource Packs",
                 "Shared resource packs folder (~/.minecraft/resourcepacks).\n"
                           "Download a resource pack .zip and put it here —\n"
                           "it will appear in the in-game resource pack list.",
          "Memory allocation:",
         "Selected: %d MB",
                  "Play",
               "Report bug on Discord",
                 "Ready",
             "Preparing...",
           "Installing Vanilla %s...",
        "Installing Fabric for %s...",
             "Fabric installed: %s",
               "Installation failed! Check the terminal.",
         "Installing Sodium / Lithium / FerriteCore...",
                  "Mods failed (see terminal), launching without them",
             "Launching %s...",
         "Game launched!",
         "Launch failed! Check the terminal.",
          "TTF font not found - Russian UI would show question marks (staying English)",
         "Invalid characters removed (A-Z, a-z, 0-9, _ only)",
    };
    return L;
}

static const L10n& l10n_ru() {
    static const L10n L{
                "CЛаунчер",
             "Язык",
             "Никнейм",
              "Версия",
           "Загрузчик модов:",
            "Оптимизация: Sodium + Lithium + FerriteCore",
        "Sodium — FPS рендера, Lithium — игровая логика/тики,\n"
                         "FerriteCore — потребление памяти.\n"
                         "Версии подбираются автоматически под выбранную версию Minecraft.",
             "Моды требуют загрузчик Fabric (выберите выше)",
         "Ванильный Minecraft не умеет загружать моды.\n"
                                 "Переключите Mod Loader на Fabric.",
             "Fabric не поддерживается для 26.x (Vanilla работает)",
         "Mojang изменили архитектуру клиента в 26.x,\n"
                         "и поддержка Fabric для этой линейки сознательно вырезана.\n"
                         "Для модов используйте версию 1.21.x или старше.",
              "Мои моды",
          "Папка модов этой Fabric-версии.\n"
                         "Кидайте сюда свои .jar — они загрузятся при запуске.\n"
                         "У каждой версии своя папка — моды не конфликтуют.",
         "Fabric для %s ещё не установлена — сначала нажмите Play",
           "Fabric не поддерживается для %s — используйте 1.21.x или старше",
         "Ресурспаки",
                 "Общая папка ресурспаков (~/.minecraft/resourcepacks).\n"
                           "Скачайте .zip ресурспака и положите сюда —\n"
                           "он появится в списке в настройках игры.",
          "Выделение памяти:",
         "Выбрано: %d МБ",
                  "Играть",
               "Сообщить об ошибке в Discord",
                 "Готов к запуску",
             "Подготовка...",
           "Установка Vanilla %s...",
        "Установка Fabric для %s...",
             "Fabric установлена: %s",
               "Ошибка установки! Проверьте терминал.",
         "Установка Sodium / Lithium / FerriteCore...",
                  "Моды не установились (см. терминал), запускаю без них",
             "Запуск %s...",
         "Игра запущена!",
         "Ошибка запуска! Проверьте терминал.",
          "Не найден TTF-шрифт — русский интерфейс покажет вопросики (остаюсь на английском)",
         "Убраны недопустимые символы (только A-Z, a-z, 0-9, _)",
    };
    return L;
}

static Launcher* g_active_launcher = nullptr;

static const char* ru_stage(const char* stage) {
    static const std::map<std::string, const char*> M = {
        {"Version metadata...", "Метаданные версии..."},
        {"Downloading client...", "Скачивание клиента..."},
        {"Libraries", "Библиотеки"},
        {"Assets", "Ассеты"},
        {"Mods", "Моды"},
        {"Natives...", "Нативные библиотеки..."},
        {"Done", "Готово"},
        {"Installing Fabric...", "Установка Fabric..."},
        {"Fabric installed", "Fabric установлена"},
    };
    auto it = M.find(stage);
    return it != M.end() ? it->second : stage;
}

static void installer_progress_cb(int percent, const char* stage, void* ) {
    if (!g_active_launcher) return;
    const char* s = (g_active_launcher->ui_lang() == UiLang::Ru) ? ru_stage(stage) : stage;
    g_active_launcher->set_status(s, percent / 100.0f);
}

Launcher::Launcher() {
    glfwSetErrorCallback([](int err, const char* desc) {
        std::cerr << "GLFW Error " << err << ": " << desc << "\n";
    });

    if (!glfwInit()) {
        throw std::runtime_error("GLFW init failed");
    }
}

Launcher::~Launcher() {

    if (worker_.joinable()) worker_.join();

    if (window_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

void Launcher::init() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window_ = glfwCreateWindow(700, 550, "CLauncher", nullptr, nullptr);
    if (!window_) throw std::runtime_error("GLFW window creation failed");

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    setup_imgui();
    load_settings();

    std::cout << "Initializing version manager...\n";
    if (!VersionManager::fetch_and_cache_manifest()) {
        std::cout << "Using cached manifest\n";
    }
    release_versions_ = VersionManager::get_release_versions();

    if (!release_versions_.empty()) {
        config_.selected_version = release_versions_[0];
    } else {
        std::cerr << "WARNING: No release versions found!\n";
    }

    config_.memory_mb = RAM_OPTIONS[memory_index_];
    set_status(tr().ready, 0.0f);
}

void Launcher::setup_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(8, 8);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 0.0f;




    ImFont* font = nullptr;
    static const char* kFontCandidates[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
#endif
    };
    for (const char* path : kFontCandidates) {
        if (FILE* f = fopen(path, "rb")) {
            fclose(f);
            font = io.Fonts->AddFontFromFileTTF(path, 18.0f, nullptr,
                                                io.Fonts->GetGlyphRangesCyrillic());
            if (font) {
                std::cout << "UI font: " << path << " (Latin + Cyrillic)\n";
                break;
            }
        }
    }
    if (!font) {
        io.Fonts->AddFontDefault();
        std::cerr << "WARNING: " << l10n_ru().font_warning << "\n";
    }

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 130");
}

void Launcher::render() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw_ui();

    ImGui::Render();
    int display_w, display_h;
    glfwGetFramebufferSize(window_, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(
        ((clear_color_ >> 0) & 0xFF) / 255.f,
        ((clear_color_ >> 8) & 0xFF) / 255.f,
        ((clear_color_ >> 16) & 0xFF) / 255.f,
        1.f
    );
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
    glfwPollEvents();

    if (glfwWindowShouldClose(window_)) should_close_ = true;
}

void Launcher::set_status(const std::string& text, float progress) {
    std::lock_guard<std::mutex> lk(status_mutex_);
    status_text = text;
    this->progress = progress;
}

const L10n& Launcher::tr() const {
    return lang_ == UiLang::Ru ? l10n_ru() : l10n_en();
}

static fs::path settings_path() {
    return launcher_paths::launcher_dir() / "launcher_settings.json";
}

void Launcher::load_settings() {
    try {
        fs::path p = settings_path();
        if (!fs::exists(p)) return;
        json j;
        std::ifstream f(p);
        f >> j;

        if (j.value("lang", "en") == "ru") lang_ = UiLang::Ru;
        std::string nick = j.value("nickname", std::string("Steve"));


        std::string clean = sanitize_nickname(nick);
        if (clean != nick) {
            std::cout << "Settings: nickname sanitized '" << nick
                      << "' -> '" << clean << "'\n";
            if (clean.empty()) clean = "Steve";
        }
        std::snprintf(nickname_buf_, sizeof(nickname_buf_), "%s", clean.c_str());
        memory_index_ = std::clamp(j.value("memory_index", 1), 0, RAM_OPTIONS_COUNT - 1);
        config_.memory_mb = RAM_OPTIONS[memory_index_];
        std::cout << "Settings loaded: lang=" << j.value("lang", "en")
                  << ", nickname=" << nickname_buf_ << "\n";
        if (clean != nick) save_settings();
    } catch (const std::exception& e) {
        std::cerr << "Settings load failed: " << e.what() << "\n";
    }
}

void Launcher::save_settings() const {
    try {
        json j;
        j["lang"] = (lang_ == UiLang::Ru) ? "ru" : "en";
        j["nickname"] = std::string(nickname_buf_);
        j["memory_index"] = memory_index_;
        fs::create_directories(settings_path().parent_path());
        std::ofstream f(settings_path());
        f << j.dump(2);
    } catch (const std::exception& e) {
        std::cerr << "Settings save failed: " << e.what() << "\n";
    }
}

void Launcher::draw_ui() {
    const L10n& L = tr();

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_FirstUseEver);

    ImGui::Begin("##launcher_main", nullptr,
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoTitleBar);


    ImGui::Text("%s", L.title.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 190);
    ImGui::SetNextItemWidth(170);
    const char* langs[] = {"English", "Русский"};
    int cur_lang = static_cast<int>(lang_);
    if (ImGui::Combo("##lang", &cur_lang, langs, IM_ARRAYSIZE(langs))) {
        lang_ = static_cast<UiLang>(cur_lang);
        save_settings();
        status_text = tr().ready;
    }
    ImGui::Separator();



    auto nick_filter = [](ImGuiInputTextCallbackData* data) -> int {
        const unsigned char c = static_cast<unsigned char>(data->EventChar);
        if (!nickname_char_ok(c)) return 1;
        return 0;
    };
    ImGui::InputText("##nickname", nickname_buf_, sizeof(nickname_buf_),
                     ImGuiInputTextFlags_CallbackCharFilter, nick_filter, nullptr);
    if (ImGui::IsItemEdited()) {
        std::string clean = sanitize_nickname(nickname_buf_);
        if (clean != nickname_buf_) {
            std::snprintf(nickname_buf_, sizeof(nickname_buf_), "%s", clean.c_str());
            ++invalid_nick_drops_;
        }
    }
    if (invalid_nick_drops_ > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", invalid_nick_drops_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", (lang_ == UiLang::Ru
                ? l10n_ru().invalid_nick_tip
                : l10n_en().invalid_nick_tip).c_str());
    }
    ImGui::SameLine();
    ImGui::Text("%s", L.nickname.c_str());

    if (ImGui::BeginCombo("##version_combo", config_.selected_version.c_str())) {
        for (const auto& ver : release_versions_) {
            bool is_selected = (ver == config_.selected_version);
            if (ImGui::Selectable(ver.c_str(), is_selected)) {
                config_.selected_version = ver;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Text("%s", L.version.c_str());

    ImGui::Spacing();

    ImGui::Text("%s", L.mod_loader.c_str());
    static const char* loader_names[] = { "Vanilla", "Fabric" };
    int current_loader = static_cast<int>(config_.mod_loader);
    if (ImGui::Combo("##loader_combo", &current_loader, loader_names, IM_ARRAYSIZE(loader_names))) {
        config_.mod_loader = static_cast<ModLoader>(current_loader);
    }
    ImGui::Spacing();


    if (config_.mod_loader == ModLoader::Fabric) {
        ImGui::Checkbox(L.perf_mods.c_str(), &perf_mods_checked_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L.perf_mods_tip.c_str());
    }


    if (!is_working) {
        if (config_.mod_loader == ModLoader::Fabric) {
            if (ImGui::Button(L.my_mods.c_str(), ImVec2(340, 0))) {
                std::string fabric_dir = MinecraftInstaller::findFabricDir(config_.selected_version);



                if (mcMajor(config_.selected_version) >= 26) {
                    std::cerr << "[My Mods] Fabric is intentionally not supported for "
                              << config_.selected_version << " (26.x line)\n";
                    set_status(sfmt(L.fabric_unsupported, config_.selected_version), 0.0f);
                } else if (fabric_dir.empty()) {
                    std::cerr << "[My Mods] Fabric is not installed for "
                              << config_.selected_version
                              << " — press Play (Fabric) first\n";
                    set_status(sfmt(L.fabric_not_installed, config_.selected_version), 0.0f);
                } else {
                    fs::path mods = launcher_paths::minecraft_dir()
                                    / "versions" / fabric_dir / "mods";
                    std::error_code ec;
                    fs::create_directories(mods, ec);
                    JavaLauncher::open_url(mods.string());
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L.my_mods_tip.c_str());
            ImGui::SameLine();
        } else {
            ImGui::TextDisabled("%s", L.mods_need_fabric.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L.mods_need_fabric_tip.c_str());
        }
        if (ImGui::Button(L.resource_packs.c_str(), ImVec2(-1, 0))) {
            fs::path rp = launcher_paths::minecraft_dir() / "resourcepacks";
            std::error_code ec;
            fs::create_directories(rp, ec);
            JavaLauncher::open_url(rp.string());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L.rp_tip.c_str());
    }

    ImGui::Text("%s", L.memory_alloc.c_str());
    ImGui::RadioButton("512 MB##ram", &memory_index_, 0); ImGui::SameLine();
    ImGui::RadioButton("1024 MB##ram", &memory_index_, 1); ImGui::SameLine();
    ImGui::RadioButton("2048 MB##ram", &memory_index_, 2); ImGui::SameLine();
    ImGui::RadioButton("3072 MB##ram", &memory_index_, 3); ImGui::SameLine();
    ImGui::RadioButton("4096 MB##ram", &memory_index_, 4);

    config_.memory_mb = RAM_OPTIONS[memory_index_];
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), L.selected_mb_fmt.c_str(), config_.memory_mb);
        ImGui::Text("%s", buf);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();


    {
        std::string st;
        float pr;
        {
            std::lock_guard<std::mutex> lk(status_mutex_);
            st = status_text;
            pr = progress;
        }
        if (is_working) {
            ImGui::Text("%s", st.c_str());
            ImGui::ProgressBar(pr, ImVec2(-1, 20));
        } else {
            if (!st.empty()) ImGui::TextDisabled("%s", st.c_str());
            if (ImGui::Button(L.play.c_str(), ImVec2(-1, 40))) {
                config_.nickname = nickname_buf_;
                save_settings();
                handle_play_button();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::Button(L.discord.c_str(), ImVec2(-1, 0))) {
        JavaLauncher::open_url(config_.discord_url);
    }

    ImGui::End();
}

void Launcher::handle_play_button() {
    if (config_.selected_version.empty()) {
        std::cerr << "Error: No version selected\n";
        return;
    }
    if (is_working) return;
    if (worker_.joinable()) worker_.join();

    is_working = true;
    set_status(tr().preparing, 0.0f);


    g_active_launcher = this;
    MinecraftInstaller::set_progress_callback(&installer_progress_cb, nullptr);


    Config cfg = config_;
    bool install_mods = (config_.mod_loader == ModLoader::Fabric) && perf_mods_checked_;
    const L10n* L = &tr();

    worker_ = std::thread([this, cfg, install_mods, L] { play_worker(cfg, install_mods, *L); });
}

void Launcher::play_worker(Launcher::Config cfg, bool install_mods, const L10n& L) {
    const std::string mc_version = cfg.selected_version;
    std::string launch_version = mc_version;

    bool install_ok = false;

    if (cfg.mod_loader == ModLoader::Vanilla) {
        set_status(sfmt(L.installing_vanilla, mc_version), 0.05f);
        install_ok = MinecraftInstaller::install(mc_version);
        launch_version = mc_version;
    } else {
        set_status(sfmt(L.installing_fabric_for, mc_version), 0.05f);
        std::string fabric_ver = MinecraftInstaller::installFabric(mc_version);
        if (!fabric_ver.empty()) {
            install_ok = true;
            launch_version = fabric_ver;
            set_status(sfmt(L.fabric_installed, launch_version), 0.40f);
        } else {
            install_ok = false;
        }
    }

    if (!install_ok) {
        set_status(L.install_failed, 0.0f);
        is_working = false;
        return;
    }


    if (install_mods) {
        set_status(L.installing_perf_mods, 0.45f);
        if (!MinecraftInstaller::installPerformanceMods(launch_version, mc_version)) {
            set_status(L.mods_failed, 0.90f);
        }
    }

    set_status(sfmt(L.launching, launch_version), 0.95f);

    bool launched = JavaLauncher::launch(cfg.nickname, launch_version, cfg.memory_mb);

    if (launched) {
        set_status(L.game_launched, 1.0f);


        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        should_close_ = true;
    } else {
        set_status(L.launch_failed, 0.0f);
    }
    is_working = false;
}

bool Launcher::should_close() const {
    return should_close_;
}
