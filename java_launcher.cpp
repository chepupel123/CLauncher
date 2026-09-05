#include "java_launcher.h"
#include "JavaManager.h"

#include <cstdlib>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <chrono>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

#ifdef _WIN32
static const char CLASSPATH_SEP = ';';
#else
static const char CLASSPATH_SEP = ':';
#endif

static std::string getOptimizationFlags(int javaVersion) {
    if (javaVersion >= 17) {
        return "-XX:+UseG1GC -XX:+ParallelRefProcEnabled -XX:MaxGCPauseMillis=20 "
               "-XX:+UnlockExperimentalVMOptions -XX:+AlwaysPreTouch -XX:G1NewSizePercent=30 "
               "-XX:G1MaxNewSizePercent=40 -XX:G1HeapRegionSize=32m -XX:G1ReservePercent=20 "
               "-XX:G1HeapWastePercent=5 -XX:G1MixedGCCountTarget=4 -XX:InitiatingHeapOccupancyPercent=15 "
               "-XX:G1MixedGCLiveThresholdPercent=90 -XX:G1RSetUpdatingPauseTimePercent=5 "
               "-XX:SurvivorRatio=32 -XX:+PerfDisableSharedMem -XX:MaxTenuringThreshold=1";
    } else {
        // Java 8: обычный набор G1-флагов. Комбинация UseParallelGC + UseStringDeduplication
        // из старой версии не запускала JVM вовсе (UseStringDeduplication требует G1).
        return "-XX:+UseG1GC -XX:MaxGCPauseMillis=50 -XX:+UseStringDeduplication "
               "-XX:G1NewSizePercent=20 -XX:G1ReservePercent=20";
    }
}

// Разбор maven-координаты "group:artifact:version[:classifier]"
static bool parse_maven_name(const std::string& lib_name, std::string& group_path,
                             std::string& artifact, std::string& lib_version,
                             std::string& classifier) {
    std::vector<std::string> parts;
    size_t start = 0, end = 0;
    while ((end = lib_name.find(':', start)) != std::string::npos) {
        parts.push_back(lib_name.substr(start, end - start));
        start = end + 1;
    }
    parts.push_back(lib_name.substr(start));
    if (parts.size() < 3) return false;

    group_path = parts[0];
    std::replace(group_path.begin(), group_path.end(), '.', '/');
    artifact = parts[1];
    lib_version = parts[2];
    classifier = (parts.size() > 3) ? parts[3] : "";
    return true;
}

// Минимальная проверка "rules" из ванильного json (OS-зависимые библиотеки,
// например twitch-* только для osx — на Linux их быть не должно и это не ошибка).
static bool library_allowed(const json& lib) {
    if (!lib.contains("rules") || !lib["rules"].is_array()) return true;

    bool allowed = false;
    for (const auto& rule : lib["rules"]) {
        if (!rule.contains("action")) continue;

        bool os_match = true;
        if (rule.contains("os") && rule["os"].contains("name")) {
#ifdef __APPLE__
            const std::string current_os = "osx";
#else
            const std::string current_os = "linux";
#endif
            os_match = (rule["os"]["name"].get<std::string>() == current_os);
        }
        if (os_match) {
            allowed = (rule["action"].get<std::string>() == "allow");
        }
    }
    return allowed;
}

// Спрашивает у java-бинарника его реальную мажорную версию ("java -version").
// Это надёжнее эвристики по пути: JavaManager может вернуть и /usr/bin/java,
// у которого в пути нет "javaNN". Возвращает -1, если определить не удалось.
static int query_java_major_version(const fs::path& java_path) {
#ifndef _WIN32
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);  // java -version пишет версию в stderr
        close(pipefd[1]);
        execl(java_path.c_str(), java_path.c_str(), "-version", (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);

    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    // парсим: version "21.0.4" или version "1.8.0_392"
    size_t p = out.find("version \"");
    if (p == std::string::npos) return -1;
    p += 9;
    std::string v;
    while (p < out.size() && (std::isdigit((unsigned char)out[p]) || out[p] == '.' || out[p] == '_'))
        v += out[p++];
    if (v.empty()) return -1;

    try {
        if (v.size() >= 2 && v[0] == '1' && v[1] == '.') {   // legacy: 1.8.0 → 8
            size_t dot = v.find('.', 2);
            return dot == std::string::npos ? 8 : std::stoi(v.substr(2, dot - 2));
        }
        size_t dot = v.find('.');
        return std::stoi(dot == std::string::npos ? v : v.substr(0, dot));
    } catch (...) {
        return -1;
    }
#else
    (void)java_path;
    return -1;
#endif
}

// ПОСЛЕДНЯЯ ЛИНИЯ ОБОРОНЫ: ник попадает в --username, в детерминированный UUID,
// в level.dat миров и в логи. UI уже фильтрует ввод, но здесь страховка от
// любых путей обхода (старый фронтенд, ручной вызов): разрешены только
// A-Z a-z 0-9 _ (правила ников Mojang), всё прочее — вырезается (UTF-8-целиком).
static std::string sanitize_username(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char b = static_cast<unsigned char>(in[i]);
        size_t len = (b < 0x80) ? 1 : (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;
        if (b < 0x80) {
            char c = static_cast<char>(b);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_')
                out += c;
        }
        i += len;
    }
    if (out.empty()) out = "Player"; // никогда не оставляем ник пустым
    return out;
}

// Детерминированный офлайн-UUID от ника (схема серверов: "OfflinePlayer:<ник>").
// Игра использует его в менеджере ресурсов: без --uuid сессия игрока неполна,
// и (наблюдение проекта) в 1.13+ не строится список языков.
static std::string offline_uuid(const std::string& nick) {
    auto fnv = [](unsigned long long h, const std::string& data) {
        for (unsigned char c : data) { h ^= c; h *= 1099511628211ULL; }
        return h;
    };
    unsigned long long hi = fnv(14695981039346656037ULL, "OfflinePlayer:" + nick);
    unsigned long long lo = fnv(14695981039346656037ULL, nick + ":offline-launcher");
    char buf[40];
    std::snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%012llx",
        (unsigned)(hi >> 32), (unsigned)((hi >> 16) & 0xFFFF),
        (unsigned)(((hi >> 12) & 0xF) | 0x2),         // версия 2 (имя-based)
        (unsigned)(((lo >> 14) & 0x3FFF) | 0x8000),   // variant bits
        (unsigned long long)((lo >> 2) & 0xFFFFFFFFFFFFULL));
    return buf;
}

// Запуск java без shell (аналог QProcess у PrismLauncher): нет инъекций через ник/пути,
// нет лимитов command line у /bin/sh. Возвращает код выхода процесса.
#ifdef _WIN32
// Конвертации UTF-8 <-> UTF-16 (пути Windows с кириллицей/эмодзи)
static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring get_launcher_base_dir_w() {
    const char* appdata = std::getenv("APPDATA");
    std::wstring base = appdata ? utf8_to_wide(appdata) : L"C:\\";
    return base + L"\\minecraft-launcher";
}
#endif

// close_on_launch: true = не ждать выхода игры (лаунчер закрывается сразу).
// PID игры дописывается в ~/.minecraft-launcher/last_game.pid (Linux) или
// %APPDATA%\minecraft-launcher\last_game.pid (Windows; pid — как DWORD).
static int run_java(const fs::path& java_path, const std::vector<std::string>& args,
                    bool close_on_launch = false) {
#ifdef _WIN32
    // ---- Windows: CreateProcessW (UTF-16-safe, пути с кириллицей ок) ----
    // Сборка командной строки с кавычками (правила Windows CRT):
    std::wstring quote = L"\"";
    std::wstring cmd = quote + java_path.wstring() + L"\'";
    for (const auto& a : args) {
        cmd += L" \"";
        // Внутри кавычек экранируем только кавычки (наши аргументы не содержат
        // обратных слэшей перед кавычками, но перебдим)
        for (wchar_t ch : utf8_to_wide(a)) {
            if (ch == L'"') cmd += L"\\\"";
            cmd += ch;
        }
        cmd += L"\'";
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        std::cerr << "CreateProcess failed: " << GetLastError() << "\n";
        return -1;
    }
    CloseHandle(pi.hThread);
    if (close_on_launch) {
        // 3 секунды на старт; если умер — код возврата
        DWORD wait = WaitForSingleObject(pi.hProcess, 3000);
        if (wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            return static_cast<int>(code);
        }
        std::ofstream pidf(wide_to_utf8(get_launcher_base_dir_w()) + "/last_game.pid");
        pidf << pi.dwProcessId << "\n";
        CloseHandle(pi.hProcess);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    const std::string java_str = java_path.string();

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(java_str.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "ERROR: fork() не удался\n";
        return -1;
    }
    if (pid == 0) {
        execv(java_str.c_str(), argv.data());
        std::cerr << "ERROR: не удалось запустить " << java_str << "\n";
        _exit(127);
    }

    if (close_on_launch) {
        // Даём JVM 3 секунды на старт; если процесс умер мгновенно —
        // сообщаем об ошибке (не закрываем лаунчер над упавшей игрой).
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int st = 0;
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) {
                if (WIFEXITED(st)) return WEXITSTATUS(st);
                if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
                return -1;
            }
        }
        // Жив после 3 секунд — успех; сохраняем PID и НЕ ждём выхода
        {
            const char* home = std::getenv("HOME");
            if (home) {
                std::ofstream pidf(std::string(home) + "/.minecraft-launcher/last_game.pid");
                pidf << pid << "\n";
            }
        }
        return 0;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif // _WIN32
}

bool JavaLauncher::launch(const std::string& nickname_raw,
                          const std::string& version,
                          int memory_mb) {

    const std::string nickname = sanitize_username(nickname_raw);

    const char* home = std::getenv("HOME");
    if (!home) {
        std::cerr << "ERROR: HOME не установлена\n";
        return false;
    }

    fs::path minecraft_dir = fs::path(home) / ".minecraft";
    fs::path versions_dir = minecraft_dir / "versions";
    fs::path version_dir = versions_dir / version;

    JavaManager javaManager;
    std::string java_path;
    try {
        java_path = javaManager.ensureJava(version).string();
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Не удалось получить Java: " << e.what() << "\n";
        return false;
    }
    std::cout << "✓ Java: " << java_path << "\n";

    int javaVersion = query_java_major_version(fs::path(java_path));
    if (javaVersion <= 0) {
        // fallback — старая эвристика по пути (если бинарник не ответил)
        javaVersion = 17;
        if (java_path.find("java8") != std::string::npos) javaVersion = 8;
        else if (java_path.find("java17") != std::string::npos) javaVersion = 17;
        else if (java_path.find("java21") != std::string::npos) javaVersion = 21;
        std::cout << "WARNING: не удалось спросить версию у java, эвристика по пути: "
                  << javaVersion << "\n";
    }
    std::cout << "✓ Java version: " << javaVersion << "\n";

    bool isFabric = (version.find("fabric-loader-") == 0);

    // ---- Читаем профиль версии (для Fabric это json от Fabric Meta) ----
    json profile;
    std::string mainClass = "net.minecraft.client.main.Main";
    std::string assetIndex = "legacy";
    std::string mcVersion = version;

    fs::path profile_json = version_dir / (version + ".json");
    if (fs::exists(profile_json)) {
        std::ifstream file(profile_json);
        if (file.is_open()) {
            try {
                file >> profile;
                if (profile.contains("mainClass")) {
                    mainClass = profile["mainClass"].get<std::string>();
                    std::cout << "✓ MainClass from profile: " << mainClass << "\n";
                }
                if (profile.contains("assetIndex") && profile["assetIndex"].contains("id")) {
                    assetIndex = profile["assetIndex"]["id"].get<std::string>();
                    std::cout << "✓ AssetIndex from profile: " << assetIndex << "\n";
                }
                // Правильный способ узнать ванильную версию — inheritsFrom,
                // а не разбор имени "fabric-loader-X-<mcVersion>" (оставлен как fallback).
                if (isFabric) {
                    if (profile.contains("inheritsFrom")) {
                        mcVersion = profile["inheritsFrom"].get<std::string>();
                    } else {
                        size_t lastDash = version.rfind('-');
                        if (lastDash != std::string::npos) {
                            mcVersion = version.substr(lastDash + 1);
                        }
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "WARNING: Ошибка парсинга profile.json: " << e.what() << "\n";
            }
        }
    } else {
        std::cerr << "WARNING: profile.json не найден, используем fallback\n";
    }

    // ---- ФИКС assetIndex (наследование) ----
    // Fabric Meta отдаёт профили БЕЗ секции assetIndex (она наследуется от
    // ванильной версии). Без индекса мы падали в fallback "legacy" →
    // Can't open resource index legacy.json → пустой список языков, серый фон
    // меню, Missing sound ×500 (ResourceManager строится из индекса).
    // Правильно: взять assetIndex из inheritsFrom-версии (как делает официальный
    // лаунчер и Prism).
    {
        std::string inherit = mcVersion; // для Fabric это уже ванильная версия
        if (assetIndex == "legacy" && !inherit.empty()) {
            fs::path vjson = versions_dir / inherit / (inherit + ".json");
            if (fs::exists(vjson)) {
                try {
                    json vj;
                    std::ifstream vf(vjson);
                    vf >> vj;
                    if (vj.contains("assetIndex") && vj["assetIndex"].contains("id")) {
                        assetIndex = vj["assetIndex"]["id"].get<std::string>();
                        std::cout << "✓ AssetIndex inherited from " << inherit
                                  << ": " << assetIndex << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cerr << "WARNING: cannot read inheritsFrom assetIndex: "
                              << e.what() << "\n";
                }
            }
        }
    }

    if (isFabric) {
        mainClass = "net.fabricmc.loader.impl.launch.knot.KnotClient";
        std::cout << "✓ Fabric main class forced: " << mainClass << "\n";
    }

    fs::path vanilla_dir = versions_dir / mcVersion;

    // Лямбда для поиска библиотеки:
    // сначала в папке Fabric версии, затем в папке ванильной версии, затем в глобальной папке
    auto find_library = [&](const std::string& group_path, const std::string& artifact,
                            const std::string& lib_version, const std::string& classifier) -> fs::path {
        std::string jar_name = artifact + "-" + lib_version + (classifier.empty() ? "" : "-" + classifier) + ".jar";
        fs::path lib_path = version_dir / "libraries" / group_path / artifact / lib_version / jar_name;
        if (fs::exists(lib_path)) return lib_path;
        lib_path = vanilla_dir / "libraries" / group_path / artifact / lib_version / jar_name;
        if (fs::exists(lib_path)) return lib_path;
        lib_path = minecraft_dir / "libraries" / group_path / artifact / lib_version / jar_name;
        if (fs::exists(lib_path)) return lib_path;
        return {};
    };

    // Добавление библиотек из json-профиля в classpath (в стиле PrismLauncher:
    // библиотеки идут в classpath, отсутствие библиотеки — фатальная ошибка, а не WARNING).
    std::vector<std::string> classpath_entries;
    std::vector<std::string> missing_libs;

    auto add_libraries_from = [&](const json& prof) {
        if (prof.is_null() || !prof.contains("libraries")) return;
        for (const auto& lib : prof["libraries"]) {
            if (!lib.contains("name")) continue;
            if (!library_allowed(lib)) continue; // OS-специфичные библиотеки других ОС

            std::string lib_name = lib["name"].get<std::string>();
            std::string group_path, artifact, lib_version, classifier;
            if (!parse_maven_name(lib_name, group_path, artifact, lib_version, classifier)) continue;

            fs::path lib_path = find_library(group_path, artifact, lib_version, classifier);
            if (!lib_path.empty()) {
                classpath_entries.push_back(lib_path.string());
            } else {
                missing_libs.push_back(lib_name);
                std::string url = lib.contains("url") && lib["url"].is_string()
                                      ? lib["url"].get<std::string>() : "";
                std::cerr << "WARNING: Библиотека не найдена: " << lib_name
                          << " (" << (group_path + "/" + artifact + "/" + lib_version)
                          << (url.empty() ? ")" : ", источник: " + url + ")");
                std::cerr << "\n";
            }
        }
    };

    // ---- Сборка classpath в стиле PrismLauncher (LaunchProfile::getLibraryFiles) ----
    // Порядок важен: сначала ВСЕ библиотеки, главный jar — ПОСЛЕДНИМ.
    // ГЛАВНЫЙ jar для Fabric — ванильный client.jar (inheritsFrom), а НЕ fabric version jar!
    // Fabric version jar в classpath не попадает НИКОГДА: это копия fabric-loader, и её
    // наличие в -cp приводит к "trying to load ... from target class loader".

    if (isFabric) {
        // 1. Библиотеки Fabric (loader, asm, sponge-mixin, intermediary) из fabric-json
        add_libraries_from(profile);

        // Критичные для Fabric библиотеки — без них запуск невозможен
        bool has_loader = false, has_mixin = false, has_asm = false, has_intermediary = false;
        if (!profile.is_null() && profile.contains("libraries")) {
            for (const auto& lib : profile["libraries"]) {
                if (!lib.contains("name")) continue;
                std::string n = lib["name"].get<std::string>();
                if (n.find(":fabric-loader:") != std::string::npos) has_loader = true;
                else if (n.find(":sponge-mixin:") != std::string::npos) has_mixin = true;
                else if (n.find(":intermediary") != std::string::npos) has_intermediary = true;
                else if (n.find(":asm") != std::string::npos) has_asm = true;
            }
        }
        if (!has_loader || !has_mixin || !has_asm || !has_intermediary) {
            std::cerr << "ERROR: Не хватает критичных библиотек Fabric:\n";
            if (!has_loader)       std::cerr << "  - net.fabricmc:fabric-loader\n";
            if (!has_mixin)        std::cerr << "  - net.fabricmc:sponge-mixin\n";
            if (!has_asm)          std::cerr << "  - org.ow2.asm:asm (и семейство)\n";
            if (!has_intermediary) std::cerr << "  - net.fabricmc:intermediary\n";
            return false;
        }

        // 2. Ванильные библиотеки из inheritsFrom-профиля
        fs::path vanilla_json_path = vanilla_dir / (mcVersion + ".json");
        if (fs::exists(vanilla_json_path)) {
            std::ifstream v_file(vanilla_json_path);
            json v_profile;
            if (v_file.is_open()) {
                try {
                    v_file >> v_profile;
                    add_libraries_from(v_profile);
                    std::cout << "✓ Ванильные библиотеки добавлены\n";
                } catch (const std::exception& e) {
                    std::cerr << "ERROR: Ошибка парсинга ванильного JSON: " << e.what() << "\n";
                    return false;
                }
            }
        } else {
            std::cerr << "ERROR: Ванильный JSON не найден: " << vanilla_json_path << "\n";
            return false;
        }
    } else {
        // Vanilla: библиотеки из профиля
        add_libraries_from(profile);
    }

    // 3. Главный jar ПОСЛЕДНИМ — всегда ванильный client.jar
    fs::path main_jar = vanilla_dir / (mcVersion + ".jar");
    if (!fs::exists(main_jar)) {
        std::cerr << "ERROR: Ванильный JAR не найден: " << main_jar << "\n";
        return false;
    }
    classpath_entries.push_back(main_jar.string());

    // Фатальная проверка отсутствующих библиотек (Prism прерывает запуск так же)
    if (!missing_libs.empty()) {
        std::cerr << "ERROR: Запуск прерван: " << missing_libs.size()
                  << " библиотек отсутствует на диске (скачайте их через лаунчер).\n";
        return false;
    }

    // Убираем дубликаты, сохраняя порядок
    std::vector<std::string> unique_entries;
    for (const auto& entry : classpath_entries) {
        if (std::find(unique_entries.begin(), unique_entries.end(), entry) == unique_entries.end()) {
            unique_entries.push_back(entry);
        }
    }
    classpath_entries = std::move(unique_entries);

    std::string classpath;
    for (const auto& entry : classpath_entries) {
        if (!classpath.empty()) classpath += CLASSPATH_SEP;
        classpath += entry;
    }

    fs::path natives_dir = version_dir / "natives";
    if (!fs::exists(natives_dir)) {
        fs::create_directories(natives_dir);
    }

    // ---- JVM-аргументы ----
    std::vector<std::string> jvm_args;
    jvm_args.push_back("-Xmx" + std::to_string(memory_mb) + "M");
    jvm_args.push_back("-Xms" + std::to_string(memory_mb) + "M");

    {
        std::istringstream flags(getOptimizationFlags(javaVersion));
        std::string f;
        while (flags >> f) jvm_args.push_back(f);
    }
    jvm_args.push_back("-Djava.library.path=" + natives_dir.string());
    // Метаданные лаунчера (как передаёт официальный): часть игровых систем
    // (в т.ч. загрузка ресурсов) ожидает эти свойства
    jvm_args.push_back("-Dminecraft.launcher.brand=minecraft-launcher");
    jvm_args.push_back("-Dminecraft.launcher.version=3.0");
    jvm_args.push_back("-Dfile.encoding=UTF-8");

    // Моды Fabric-версии (Sodium/Lithium/FerriteCore и любые другие) лежат в
    // versions/<версия>/mods/ и подключаются без смены gameDir — сейвы и конфиги
    // остаются в общей .minecraft. Свойство fabric.addMods поддерживает каталог
    // и грузит все *.jar внутри рекурсивно (ArgumentModCandidateFinder).
    {
        fs::path mods_dir = version_dir / "mods";
        if (fs::exists(mods_dir) && !fs::is_empty(mods_dir)) {
            jvm_args.push_back("-Dfabric.addMods=" + mods_dir.string());
            std::cout << "✓ Папка модов подключена: " << mods_dir << "\n";
        }
    }

    // ---- Аргументы игры ----
    std::vector<std::string> game_args;
    game_args.push_back("--username");
    game_args.push_back(nickname);
    game_args.push_back("--uuid");
    game_args.push_back(offline_uuid(nickname));
    game_args.push_back("--accessToken");
    game_args.push_back("null");
    game_args.push_back("--userType");
    game_args.push_back("mojang");
    game_args.push_back("--clientId");
    game_args.push_back("0");
    game_args.push_back("--xuid");
    game_args.push_back("0");
    game_args.push_back("--version");
    game_args.push_back(version);
    game_args.push_back("--gameDir");
    game_args.push_back(minecraft_dir.string());
    game_args.push_back("--assetsDir");
    game_args.push_back((minecraft_dir / "assets").string());
    game_args.push_back("--assetIndex");
    game_args.push_back(assetIndex);
    game_args.push_back("--userProperties");
    game_args.push_back("{}");
    game_args.push_back("--versionType");
    game_args.push_back("release");

    // Для Java 9+ отправляем JVM-аргументы и -cp через @argfile (как это делают
    // современные лаунчеры): не упираемся в лимит длины аргумента и в лимиты shell.
    std::vector<std::string> exec_args;
    fs::path argfile = version_dir / "java-args.argfile";
    bool use_argfile = (javaVersion >= 9);

    if (use_argfile) {
        std::ofstream af(argfile);
        if (!af.is_open()) {
            std::cerr << "ERROR: Не удалось создать argfile: " << argfile << "\n";
            return false;
        }
        for (const auto& a : jvm_args) {
            if (a.rfind("-D", 0) == 0) af << "\"" << a << "\"\n"; // пути могут содержать пробелы
            else af << a << "\n";
        }
        af << "-cp \"" << classpath << "\"\n";
        af.close();
        exec_args.push_back("@" + argfile.string());
    } else {
        for (const auto& a : jvm_args) exec_args.push_back(a);
        exec_args.push_back("-cp");
        exec_args.push_back(classpath);
    }
    exec_args.push_back(mainClass);
    for (const auto& a : game_args) exec_args.push_back(a);

    std::cout << "\n";
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "🎮 ЗАПУСК MINECRAFT\n";
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "Ник:       " << nickname << "\n";
    std::cout << "Версия:    " << version << "\n";
    if (isFabric) std::cout << "Fabric MC: " << mcVersion << "\n";
    std::cout << "Память:    " << memory_mb << " МБ\n";
    std::cout << "Java:      " << java_path << " (v" << javaVersion << ")\n";
    std::cout << "MainClass: " << mainClass << "\n";
    std::cout << "Jar в classpath: " << classpath_entries.size() << "\n";
    std::cout << "═══════════════════════════════════════════\n\n";

    if (std::getenv("LAUNCHER_DEBUG") != nullptr) {
        std::cout << "Command:";
        std::cout << " " << java_path;
        for (const auto& a : exec_args) std::cout << " \"" << a << "\"";
        std::cout << "\n\n";
    }

    // Не ждать выхода игры: JVM — отдельный процесс (execv), лаунчер закрывается
    // после успешного старта. PID пишется в ~/.minecraft-launcher/last_game.pid.
    int ret = run_java(fs::path(java_path), exec_args, /*close_on_launch=*/true);

    if (ret == 0) {
        std::cout << "✓ Игра завершила работу успешно!\n";
        return true;
    } else {
        std::cerr << "ERROR: Ошибка запуска или игра вылетела (код " << ret << ")\n";
        return false;
    }
}

void JavaLauncher::open_url(const std::string& url) {
    std::cout << "Открываем: " << url << "\n";
#ifdef _WIN32
    // ShellExecuteW откроет URL системным браузером (UTF-16-safe)
    std::wstring wurl = utf8_to_wide(url);
    HINSTANCE r = ShellExecuteW(nullptr, L"open", wurl.c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) > 32) std::cout << "✓ URL открыт\n";
    else std::cerr << "Не удалось открыть URL\n";
#else
    std::string cmd = "xdg-open \"" + url + "\" 2>/dev/null &";
    if (system(cmd.c_str()) == 0) {
        std::cout << "✓ URL открыт\n";
    }
#endif
}
