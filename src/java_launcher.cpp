#include "java_launcher.h"
#include "JavaManager.h"
#include "paths.h"

#include <cstdlib>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <fstream>
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdio>
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


        return "-XX:+UseG1GC -XX:MaxGCPauseMillis=50 -XX:+UseStringDeduplication "
               "-XX:G1NewSizePercent=20 -XX:G1ReservePercent=20";
    }
}

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

static bool library_allowed(const json& lib) {
    if (!lib.contains("rules") || !lib["rules"].is_array()) return true;

    bool allowed = false;
    for (const auto& rule : lib["rules"]) {
        if (!rule.contains("action")) continue;

        bool os_match = true;
        if (rule.contains("os") && rule["os"].contains("name")) {
#if defined(_WIN32)
            const std::string current_os = "windows";
#elif defined(__APPLE__)
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

static int query_java_major_version(const fs::path& java_path) {
#ifndef _WIN32
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
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


    size_t p = out.find("version \"");
    if (p == std::string::npos) return -1;
    p += 9;
    std::string v;
    while (p < out.size() && (std::isdigit((unsigned char)out[p]) || out[p] == '.' || out[p] == '_'))
        v += out[p++];
    if (v.empty()) return -1;

    try {
        if (v.size() >= 2 && v[0] == '1' && v[1] == '.') {
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
    if (out.empty()) out = "Player";
    return out;
}

// MD5 (RFC 1321) — используется для детерминированного офлайн-UUID по схеме
// ванильного лаунчера: MD5("OfflinePlayer:" + ник), как это делает сервер.
namespace {
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t buf_len = 0;

    static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    void process(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
        };
        static const int S[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
        };
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
                   ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D);       g = i; }
            else if (i < 32) { F = (D & B) | (~D & C);       g = (5*i + 1) & 15; }
            else if (i < 48) { F = B ^ C ^ D;                g = (3*i + 5) & 15; }
            else             { F = C ^ (B | ~D);             g = (7*i) & 15; }
            uint32_t tmp = D; D = C; C = B;
            B = B + rotl(A + F + K[i] + M[g], S[i]);
            A = tmp;
        }
        a += A; b += B; c += C; d += D;
    }

    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len += n;
        while (n > 0) {
            size_t take = std::min(n, size_t(64) - buf_len);
            std::memcpy(buf + buf_len, p, take);
            buf_len += take; p += take; n -= take;
            if (buf_len == 64) { process(buf); buf_len = 0; }
        }
    }

    void final(uint8_t out[16]) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (buf_len != 56) update(&z, 1);
        uint8_t lenb[8];
        // MD5 хранит длину сообщения в little-endian (в отличие от SHA-1).
        for (int i = 0; i < 8; ++i) lenb[i] = uint8_t(bits >> (8 * i));
        update(lenb, 8);
        for (int i = 0; i < 4; ++i) {
            out[i]      = uint8_t(a >> (i * 8));
            out[i + 4]  = uint8_t(b >> (i * 8));
            out[i + 8]  = uint8_t(c >> (i * 8));
            out[i + 12] = uint8_t(d >> (i * 8));
        }
    }
};
} // namespace

static std::string offline_uuid(const std::string& nick) {
    // Схема ванильного сервера: UUID.nameUUIDFromBytes("OfflinePlayer:" + nick)
    // = MD5-дайджест, version 3, IETF-вариант. Совпадает с UUID, который
    // сгенерировал бы официальный лаунчер, поэтому прогресс игрока
    // сохраняется между лаунчерами.
    const std::string data = "OfflinePlayer:" + nick;
    Md5 md5;
    md5.update(data.data(), data.size());
    uint8_t digest[16];
    md5.final(digest);
    digest[6] = (digest[6] & 0x0f) | 0x30; // version 3
    digest[8] = (digest[8] & 0x3f) | 0x80; // IETF variant
    char buf[37];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        digest[0], digest[1], digest[2], digest[3],
        digest[4], digest[5], digest[6], digest[7],
        digest[8], digest[9], digest[10], digest[11],
        digest[12], digest[13], digest[14], digest[15]);
    return buf;
}

#ifdef _WIN32

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
// Корректное экранирование аргумента для командной строки Windows
// (правила Microsoft для CommandLineToArgvW / CreateProcessW).
static std::wstring win_quote_arg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0; i < arg.size(); ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++backslashes; }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        } else if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}
#endif

static int run_java(const fs::path& java_path, const std::vector<std::string>& args,
                    bool close_on_launch = false) {
#ifdef _WIN32
    std::wstring cmd = win_quote_arg(java_path.wstring());
    for (const auto& a : args) {
        cmd += L" ";
        cmd += win_quote_arg(utf8_to_wide(a));
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
        // Даём процессу до 3 секунд: если он сразу упал — возвращаем код ошибки,
        // иначе считаем запуск успешным и запоминаем PID.
        DWORD wait = WaitForSingleObject(pi.hProcess, 3000);
        if (wait == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            return static_cast<int>(code);
        }
        std::error_code ec;
        fs::create_directories(launcher_paths::launcher_dir(), ec);
        std::ofstream pidf(launcher_paths::launcher_dir() / "last_game.pid");
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

        {
            std::error_code ec;
            fs::create_directories(launcher_paths::launcher_dir(), ec);
            std::ofstream pidf(launcher_paths::launcher_dir() / "last_game.pid");
            pidf << pid << "\n";
        }
        return 0;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

bool JavaLauncher::launch(const std::string& nickname_raw,
                          const std::string& version,
                          int memory_mb) {

    const std::string nickname = sanitize_username(nickname_raw);

    fs::path minecraft_dir = launcher_paths::minecraft_dir();
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

        javaVersion = 17;
        if (java_path.find("java8") != std::string::npos) javaVersion = 8;
        else if (java_path.find("java17") != std::string::npos) javaVersion = 17;
        else if (java_path.find("java21") != std::string::npos) javaVersion = 21;
        std::cout << "WARNING: не удалось спросить версию у java, эвристика по пути: "
                  << javaVersion << "\n";
    }
    std::cout << "✓ Java version: " << javaVersion << "\n";

    bool isFabric = (version.find("fabric-loader-") == 0);


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

    {
        std::string inherit = mcVersion;
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



    std::vector<std::string> classpath_entries;
    std::vector<std::string> missing_libs;

    auto add_libraries_from = [&](const json& prof) {
        if (prof.is_null() || !prof.contains("libraries")) return;
        for (const auto& lib : prof["libraries"]) {
            if (!lib.contains("name")) continue;
            if (!library_allowed(lib)) continue;

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


    if (isFabric) {

        add_libraries_from(profile);


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

        add_libraries_from(profile);
    }


    fs::path main_jar = vanilla_dir / (mcVersion + ".jar");
    if (!fs::exists(main_jar)) {
        std::cerr << "ERROR: Ванильный JAR не найден: " << main_jar << "\n";
        return false;
    }
    classpath_entries.push_back(main_jar.string());


    if (!missing_libs.empty()) {
        std::cerr << "ERROR: Запуск прерван: " << missing_libs.size()
                  << " библиотек отсутствует на диске (скачайте их через лаунчер).\n";
        return false;
    }


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


    std::vector<std::string> jvm_args;
    jvm_args.push_back("-Xmx" + std::to_string(memory_mb) + "M");
    jvm_args.push_back("-Xms" + std::to_string(memory_mb) + "M");

    {
        std::istringstream flags(getOptimizationFlags(javaVersion));
        std::string f;
        while (flags >> f) jvm_args.push_back(f);
    }
    jvm_args.push_back("-Djava.library.path=" + natives_dir.string());


    jvm_args.push_back("-Dminecraft.launcher.brand=minecraft-launcher");
    jvm_args.push_back("-Dminecraft.launcher.version=3.0");
    jvm_args.push_back("-Dfile.encoding=UTF-8");


    {
        fs::path mods_dir = version_dir / "mods";
        if (fs::exists(mods_dir) && !fs::is_empty(mods_dir)) {
            jvm_args.push_back("-Dfabric.addMods=" + mods_dir.string());
            std::cout << "✓ Папка модов подключена: " << mods_dir << "\n";
        }
    }


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
            if (a.rfind("-D", 0) == 0) af << "\"" << a << "\"\n";
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



    int ret = run_java(fs::path(java_path), exec_args, true);

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
