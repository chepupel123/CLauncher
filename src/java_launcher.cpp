#include "java_launcher.h"
#include "JavaManager.h"
#include "paths.h"
#include "logger.h"

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
#include <cctype>
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
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <spawn.h>
extern char** environ;
#endif

#include <chrono>
#include <thread>

using json = nlohmann::json;
namespace fs = std::filesystem;

#ifdef _WIN32
static const char CLASSPATH_SEP = ';';
#else
static const char CLASSPATH_SEP = ':';
#endif

static std::string getOptimizationFlags(int javaVersion) {
    if (javaVersion >= 17) {
        const unsigned gc_threads = std::max(1u, std::thread::hardware_concurrency() / 2);
        std::string flags = "-XX:+UnlockExperimentalVMOptions "
                            "-XX:+UseG1GC "
                            "-XX:MaxGCPauseMillis=100 "
                            "-XX:+ParallelRefProcEnabled "
                            "-XX:G1NewSizePercent=20 "
                            "-XX:G1MaxNewSizePercent=40 "
                            "-XX:G1HeapRegionSize=4m "
                            "-XX:G1ReservePercent=15 "
                            "-XX:G1HeapWastePercent=5 "
                            "-XX:InitiatingHeapOccupancyPercent=20 "
                            "-XX:G1MixedGCLiveThresholdPercent=90 "
                            "-XX:G1RSetUpdatingPauseTimePercent=5 "
                            "-XX:SurvivorRatio=8 "
                            "-XX:MaxTenuringThreshold=4 "
                            "-XX:+DisableExplicitGC "
                            "-XX:+PerfDisableSharedMem "
                            "-XX:ParallelGCThreads=" + std::to_string(gc_threads);
        // Compact Object Headers support is available in current JDKs (25+), not Java 21.
        if (javaVersion >= 25) flags += " -XX:+UseCompactObjectHeaders";
        return flags;
    }
    return "-XX:+UseParallelGC -XX:MaxGCPauseMillis=50 -XX:+DisableExplicitGC";
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

static int parse_java_version_output(const std::string& out) {
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
}

static int query_java_major_version(const fs::path& java_path) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    // CreateProcessW не гоняет строку через cmd.exe, поэтому 2>&1 здесь не работает.
    // stderr забирается хэндлом hStdError.
    std::wstring cmd = L"\"" + java_path.wstring() + L"\" -version";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        LOG_ERROR("CreateProcessW(java -version) failed, GetLastError=" << GetLastError());
        return -1;
    }
    CloseHandle(hWrite);

    std::string out;
    char buf[512];
    DWORD n = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0)
        out.append(buf, n);
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return parse_java_version_output(out);
#else
    std::string quoted = "'";
    for (char c : java_path.string()) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    const std::string cmd = quoted + " -version 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        LOG_ERROR("popen(java -version) failed");
        return -1;
    }
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) out.append(buf);
    pclose(pipe);
    return parse_java_version_output(out);
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
    const std::string data = "OfflinePlayer:" + nick;
    Md5 md5;
    md5.update(data.data(), data.size());
    uint8_t digest[16];
    md5.final(digest);
    digest[6] = (digest[6] & 0x0f) | 0x30;
    digest[8] = (digest[8] & 0x3f) | 0x80;
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

static fs::path game_log_path() {
    return launcher_paths::launcher_dir() / "game.log";
}

static void log_game_output_tail() {
    std::ifstream in(game_log_path());
    if (!in) {
        LOG_ERROR("game.log не найден: " << game_log_path().string());
        return;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) lines.push_back(std::move(line));
    }
    if (lines.empty()) {
        LOG_ERROR("game.log пуст: " << game_log_path().string());
        return;
    }
    LOG_ERROR("Вывод игры (" << game_log_path().string() << "):");
    size_t start = lines.size() > 40 ? lines.size() - 40 : 0;
    for (size_t i = start; i < lines.size(); ++i)
        LOG_ERROR("game: " << lines[i]);
}

static int run_java(const fs::path& java_path, const std::vector<std::string>& args,
                    bool close_on_launch = false) {
#ifdef _WIN32
    std::wstring cmd = win_quote_arg(java_path.wstring());
    for (const auto& a : args) {
        cmd += L" ";
        cmd += win_quote_arg(utf8_to_wide(a));
    }
    LOG_DEBUG("CreateProcessW cmd: " << wide_to_utf8(cmd));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::error_code ec;
    fs::create_directories(launcher_paths::launcher_dir(), ec);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE logf = CreateFileW(game_log_path().wstring().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (logf != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = logf;
        si.hStdError = logf;
    }
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                        logf != INVALID_HANDLE_VALUE,
                        CREATE_UNICODE_ENVIRONMENT,
                        nullptr, nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        LOG_ERROR("CreateProcessW failed, GetLastError=" << err);
        if (logf != INVALID_HANDLE_VALUE) CloseHandle(logf);
        return -1;
    }
    if (logf != INVALID_HANDLE_VALUE) CloseHandle(logf);
    CloseHandle(pi.hThread);
    if (close_on_launch) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 500);
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

    std::error_code mk_ec;
    fs::create_directories(launcher_paths::launcher_dir(), mk_ec);
    int log_fd = open(game_log_path().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        if (log_fd >= 0) close(log_fd);
        LOG_ERROR("posix_spawn_file_actions_init() failed");
        return -1;
    }
    if (log_fd >= 0) {
        bool actions_ok =
            posix_spawn_file_actions_adddup2(&actions, log_fd, STDOUT_FILENO) == 0 &&
            posix_spawn_file_actions_adddup2(&actions, log_fd, STDERR_FILENO) == 0 &&
            (log_fd <= 2 || posix_spawn_file_actions_addclose(&actions, log_fd) == 0);
        if (!actions_ok) {
            posix_spawn_file_actions_destroy(&actions);
            close(log_fd);
            LOG_ERROR("posix_spawn file actions failed");
            return -1;
        }
    }

    pid_t pid = 0;
    int spawn_rc = posix_spawn(&pid, java_str.c_str(), &actions, nullptr,
                               argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (log_fd >= 0) close(log_fd);
    if (spawn_rc != 0) {
        LOG_ERROR("posix_spawn() failed: " << std::strerror(spawn_rc));
        return -1;
    }

    if (close_on_launch) {
        for (int i = 0; i < 5; ++i) {
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

// Сравнение версий Minecraft ("1.9" < "1.18" и т.п.).
// Нельзя сравнивать версии как строки: "1.9" < "1.18" дало бы false.
static bool is_version_less(const std::string& v, int target_major, int target_minor, int target_patch) {
    int maj = 0, min = 0, pat = 0;
    std::sscanf(v.c_str(), "%d.%d.%d", &maj, &min, &pat);
    if (maj != target_major) return maj < target_major;
    if (min != target_minor) return min < target_minor;
    return pat < target_patch;
}

static bool launch_impl(const std::string& nickname_raw,
                        const std::string& version,
                        const std::string& mc_version_hint,
                        int memory_mb,
                        const fs::path& game_dir,
                        const fs::path& assets_dir,
                        const fs::path& mods_path,
                        const fs::path& argfile_dir,
                        const std::string& java_override,
                        bool wait_for_exit) {

    const std::string nickname = sanitize_username(nickname_raw);

    fs::path minecraft_dir = launcher_paths::shared_dir();
    fs::path versions_dir  = launcher_paths::shared_versions_dir();
    fs::path version_dir   = versions_dir / version;

    JavaManager javaManager;
    std::string java_path;
    try {
        JavaInstance inst;
        inst.major = javaManager.requiredMajor(mc_version_hint.empty() ? version : mc_version_hint);
        java_path = java_override.empty() ? javaManager.resolveJava(inst).string() : java_override;
    } catch (const std::exception& e) {
        LOG_ERROR("Не удалось получить Java: " << e.what());
        return false;
    }
    LOG_INFO("Java: " << java_path);

    int javaVersion = query_java_major_version(fs::path(java_path));
    if (javaVersion <= 0) {
        javaVersion = 17;
        if (java_path.find("java8")  != std::string::npos) javaVersion = 8;
        else if (java_path.find("java17") != std::string::npos) javaVersion = 17;
        else if (java_path.find("java21") != std::string::npos) javaVersion = 21;
        LOG_WARN("Не удалось определить версию Java, эвристика: " << javaVersion);
    }
    LOG_INFO("Java version: " << javaVersion);

    bool isFabric = (version.find("fabric-loader-") == 0);
    bool isForge = (version.find("-forge-") != std::string::npos);

    json profile;
    bool main_class_from_profile = false;
    std::string mainClass   = "net.minecraft.client.main.Main";
    std::string assetIndex  = "legacy";
    std::string mcVersion   = mc_version_hint.empty() ? version : mc_version_hint;

    fs::path profile_json = version_dir / (version + ".json");
    if (fs::exists(profile_json)) {
        std::ifstream file(profile_json);
        if (file.is_open()) {
            try {
                file >> profile;
                if (profile.contains("mainClass")) {
                    mainClass = profile["mainClass"].get<std::string>();
                    main_class_from_profile = true;
                    LOG_INFO("MainClass from profile: " << mainClass);
                }
                if (profile.contains("assetIndex") && profile["assetIndex"].contains("id")) {
                    assetIndex = profile["assetIndex"]["id"].get<std::string>();
                    LOG_INFO("AssetIndex from profile: " << assetIndex);
                }
                if (isFabric || isForge) {
                    if (profile.contains("inheritsFrom")) {
                        mcVersion = profile["inheritsFrom"].get<std::string>();
                    } else {
                        size_t forgePos = version.find("-forge-");
                        if (forgePos != std::string::npos) {
                            mcVersion = version.substr(0, forgePos);
                        } else {
                            size_t lastDash = version.rfind('-');
                            if (lastDash != std::string::npos) {
                                mcVersion = version.substr(lastDash + 1);
                            }
                        }
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN("Ошибка парсинга profile.json: " << e.what());
            }
        }
    } else {
        LOG_WARN("profile.json не найден, используем fallback");
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
                        LOG_INFO("AssetIndex inherited from " << inherit
                                 << ": " << assetIndex);
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("Cannot read inheritsFrom assetIndex: " << e.what());
                }
            }
        }
    }

    if (isForge && !main_class_from_profile) {
        LOG_ERROR("Forge profile does not define mainClass: " << profile_json.string());
        return false;
    }

    if (isFabric) {
        mainClass = "net.fabricmc.loader.impl.launch.knot.KnotClient";
        LOG_INFO("Fabric main class forced: " << mainClass);
    }

    fs::path vanilla_dir = versions_dir / mcVersion;

    auto find_library = [&](const std::string& group_path, const std::string& artifact,
                        const std::string& lib_version, const std::string& classifier) -> fs::path {
    std::string jar_name = artifact + "-" + lib_version +
                           (classifier.empty() ? "" : "-" + classifier) + ".jar";
    fs::path lib_path = version_dir / "libraries" / group_path / artifact / lib_version / jar_name;
    if (fs::exists(lib_path)) return lib_path;
    lib_path = vanilla_dir / "libraries" / group_path / artifact / lib_version / jar_name;
    if (fs::exists(lib_path)) return lib_path;
    lib_path = launcher_paths::shared_libraries_dir() / group_path / artifact / lib_version / jar_name;
    if (fs::exists(lib_path)) return lib_path;

    // Legacy Forge (1.7.10 / 1.12.2): Prism может хранить universal под
    // <mc>-<forge>-<mc>, а в name указывать <mc>-<forge>. Прямой путь тогда
    // не совпадает с файлом на диске. Ищем forge-*-<classifier>.jar в любой
    // подпапке net/minecraftforge/forge/*/.
    if (artifact == "forge" && group_path == "net/minecraftforge") {
        const fs::path bases[] = {
            version_dir / "libraries" / group_path / artifact,
            vanilla_dir / "libraries" / group_path / artifact,
            launcher_paths::shared_libraries_dir() / group_path / artifact
        };
        const std::string suffix = classifier.empty()
            ? std::string(".jar")
            : ("-" + classifier + ".jar");
        for (const auto& base : bases) {
            std::error_code ec2;
            if (!fs::exists(base, ec2)) continue;
            for (const auto& sub : fs::directory_iterator(base, ec2)) {
                if (ec2 || !sub.is_directory()) continue;
                for (const auto& f : fs::directory_iterator(sub.path(), ec2)) {
                    if (ec2 || !f.is_regular_file()) continue;
                    const std::string fn = f.path().filename().string();
                    if (fn.rfind("forge-", 0) != 0) continue;
                    if (fn.size() < suffix.size()) continue;
                    if (fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
                    return f.path();
                }
            }
        }
    }

    return {};
};

    std::vector<std::string> classpath_entries;
    std::vector<std::string> missing_libs;

    auto add_libraries_from = [&](const json& prof) {
        if (prof.is_null() || !prof.contains("libraries")) return;
        for (const auto& lib : prof["libraries"]) {
            if (!lib.contains("name")) continue;
            if (!library_allowed(lib)) continue;
            // 1.12.2: jinput-platform has only natives classifiers, no artifact jar.
            // Installer extracts it into versions/<ver>/natives. Not a classpath entry.
            if (lib.contains("natives") &&
                !(lib.contains("downloads") && lib["downloads"].contains("artifact"))) {
                continue;
            }

            std::string lib_name = lib["name"].get<std::string>();

            // Patch duplicated version for legacy Forge (Prism): "1.7.10-10.13.4.1614-1.7.10" -> "1.7.10-10.13.4.1614"
            if (lib_name.find("net.minecraftforge:forge:") != std::string::npos) {
                size_t first_colon = lib_name.find(':');
                size_t second_colon = lib_name.find(':', first_colon + 1);
                if (first_colon != std::string::npos && second_colon != std::string::npos) {
                    std::string ver_part = lib_name.substr(second_colon + 1);
                    size_t second_dash = ver_part.find('-', ver_part.find('-') + 1);
                    if (second_dash != std::string::npos && ver_part.find(":universal") != std::string::npos) {
                        lib_name = "net.minecraftforge:forge:" + ver_part.substr(0, second_dash) + ":universal";
                    }
                }
            }

            std::string group_path, artifact, lib_version, classifier;
            if (!parse_maven_name(lib_name, group_path, artifact, lib_version, classifier)) continue;

            fs::path lib_path = find_library(group_path, artifact, lib_version, classifier);
            if (!lib_path.empty()) {
                classpath_entries.push_back(lib_path.string());
            } else {
                missing_libs.push_back(lib_name);
                LOG_WARN("Библиотека не найдена: " << lib_name);
            }
        }
    };

    if (isFabric || isForge) {
        add_libraries_from(profile);

        fs::path vanilla_json_path = vanilla_dir / (mcVersion + ".json");
        if (fs::exists(vanilla_json_path)) {
            std::ifstream v_file(vanilla_json_path);
            json v_profile;
            if (v_file.is_open()) {
                try {
                    v_file >> v_profile;
                    add_libraries_from(v_profile);
                    LOG_INFO("Ванильные библиотеки добавлены");
                } catch (const std::exception& e) {
                    LOG_ERROR("Ошибка парсинга ванильного JSON: " << e.what());
                    return false;
                }
            }
        } else {
            LOG_ERROR("Ванильный JSON не найден: " << vanilla_json_path.string());
            return false;
        }
    } else {
        add_libraries_from(profile);
    }

    if (isForge) {
        // Legacy Forge (1.12.2, 1.7.10) uses LaunchWrapper and patches classes
        // from the vanilla JAR at runtime, so the vanilla JAR must be on the
        // classpath. Modern Forge (1.17+) uses JPMS and ships its own patched
        // client-srg.jar; adding the vanilla JAR there causes package conflicts.
        const bool is_legacy_forge =
            mainClass.find("launchwrapper") != std::string::npos;

        if (is_legacy_forge) {
            fs::path main_jar = vanilla_dir / (mcVersion + ".jar");
            if (!fs::exists(main_jar)) {
                LOG_ERROR("Ванильный JAR не найден: " << main_jar.string());
                return false;
            }
            classpath_entries.push_back(main_jar.string());
            LOG_INFO("Adding vanilla JAR for legacy Forge (LaunchWrapper)");
        } else {
            LOG_INFO("Skipping vanilla JAR for modern Forge (JPMS, handled by client-srg.jar)");
        }
    } else {
        fs::path main_jar = vanilla_dir / (mcVersion + ".jar");
        if (!fs::exists(main_jar)) {
            LOG_ERROR("Ванильный JAR не найден: " << main_jar.string());
            return false;
        }
        classpath_entries.push_back(main_jar.string());
    }

    if (!missing_libs.empty()) {
        LOG_ERROR("Запуск прерван: " << missing_libs.size()
                  << " библиотек отсутствует");
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
    // Нативы ставит установщик в ванильную версию (mcVersion = inheritsFrom), а не в профиль Forge.
    if (!fs::exists(natives_dir) || fs::is_empty(natives_dir)) {
        fs::path vanilla_natives = vanilla_dir / "natives";
        if (fs::exists(vanilla_natives) && !fs::is_empty(vanilla_natives))
            natives_dir = vanilla_natives;
    }
    if (!fs::exists(natives_dir)) {
        std::error_code ec;
        fs::create_directories(natives_dir, ec);
    }

    // Forge's generated profile carries required module-path flags and launchTarget
    // arguments (notably for 1.20.1+). Keep them profile-driven rather than
    // hardcoding a Forge main class or a particular Forge build's arguments.
    auto replace_profile_variables = [&](std::string& arg) {
        auto replace_all = [&](const std::string& token, const std::string& replacement) {
            size_t at = 0;
            while ((at = arg.find(token, at)) != std::string::npos) {
                arg.replace(at, token.size(), replacement);
                at += replacement.size();
            }
        };
        replace_all("${auth_player_name}", nickname);
        replace_all("${version_name}", version);
        replace_all("${game_directory}", game_dir.string());
        replace_all("${assets_root}", assets_dir.string());
        replace_all("${assets_index_name}", assetIndex);
        replace_all("${auth_uuid}", offline_uuid(nickname));
        replace_all("${auth_access_token}", "null");
        replace_all("${user_type}", "mojang");
        replace_all("${user_properties}", "{}");
        replace_all("${version_type}", "release");
        replace_all("${library_directory}", launcher_paths::shared_libraries_dir().string());
        replace_all("${classpath_separator}", std::string(1, CLASSPATH_SEP));
    };

    auto append_profile_arguments = [&](const char* key, std::vector<std::string>& target) {
        const bool has_legacy = profile.contains("minecraftArguments") &&
                                profile["minecraftArguments"].is_string();
        if (!isForge && !has_legacy) return;

        if (profile.contains("arguments")) {
            if (!profile["arguments"].is_object() || !profile["arguments"].contains(key)) return;
            const auto& values = profile["arguments"][key];
            if (!values.is_array()) return;
            for (const auto& value : values) {
                if (!value.is_string()) continue; // Forge 1.20.1 arguments are strings.
                std::string arg = value.get<std::string>();
                replace_profile_variables(arg);
                target.push_back(std::move(arg));
            }
            return;
        }

        // Legacy Forge (e.g. Minecraft 1.7.10 / 1.12.2) stores the complete
        // client argument template in minecraftArguments, including --tweakClass.
        if (std::string(key) != "game" || !profile.contains("minecraftArguments") ||
            !profile["minecraftArguments"].is_string()) return;
        std::istringstream legacy_args(profile["minecraftArguments"].get<std::string>());
        std::string arg;
        while (legacy_args >> arg) {
            replace_profile_variables(arg);
            target.push_back(std::move(arg));
        }
    };

    std::vector<std::string> jvm_args;
    jvm_args.push_back("-Xmx" + std::to_string(memory_mb) + "M");
    // -Xms = -Xmx aborts Java instantly if the machine cannot commit that much.
    jvm_args.push_back("-Xms" + std::to_string(std::min(memory_mb, 512)) + "M");
    {
        std::istringstream flags(getOptimizationFlags(javaVersion));
        std::string f;
        while (flags >> f) jvm_args.push_back(f);
    }
    jvm_args.push_back("-Djava.library.path=" + natives_dir.string());
    jvm_args.push_back("-Dminecraft.launcher.brand=CLauncher");
    jvm_args.push_back("-Dminecraft.launcher.version=3.0");
    jvm_args.push_back("-Dfile.encoding=UTF-8");
    append_profile_arguments("jvm", jvm_args);

    // CVE-2021-44228 (Log4Shell): 1.18.0 и ниже уязвимы. 1.18.1 и выше — нет.
    if (is_version_less(mcVersion, 1, 18, 1)) {
        jvm_args.push_back("-Dlog4j2.formatMsgNoLookups=true");
    }

    {
        fs::path mods_dir = mods_path;
        if (fs::exists(mods_dir) && !fs::is_empty(mods_dir)) {
            jvm_args.push_back("-Dfabric.addMods=" + mods_dir.string());
            LOG_INFO("Папка модов подключена: " << mods_dir.string());
        }
    }

        std::vector<std::string> game_args;
    const bool has_legacy_args = profile.contains("minecraftArguments") &&
                                 profile["minecraftArguments"].is_string();

    // Legacy Forge profiles provide the full argument list themselves. Avoid
    // duplicating options such as --gameDir and --username.
    if (!has_legacy_args) {
        game_args.push_back("--username");    game_args.push_back(nickname);
        game_args.push_back("--uuid");        game_args.push_back(offline_uuid(nickname));
        game_args.push_back("--accessToken"); game_args.push_back("null");
        game_args.push_back("--userType");    game_args.push_back("mojang");
        game_args.push_back("--clientId");    game_args.push_back("0");
        game_args.push_back("--xuid");        game_args.push_back("0");
        game_args.push_back("--version");     game_args.push_back(version);
        game_args.push_back("--gameDir");     game_args.push_back(game_dir.string());
        game_args.push_back("--assetsDir");   game_args.push_back(assets_dir.string());
        game_args.push_back("--assetIndex");  game_args.push_back(assetIndex);
        game_args.push_back("--userProperties"); game_args.push_back("{}");
        game_args.push_back("--versionType"); game_args.push_back("release");
        game_args.push_back("--fullScreen");
    }
    append_profile_arguments("game", game_args);

    // Legacy Forge (< 1.13): LaunchWrapper требует --tweakClass FMLTweaker.
    // Без него LaunchWrapper подставляет VanillaTweaker и падает с
    // ClassNotFoundException: net.minecraft.client.Minecraft на обфусцированном jar.
    if (isForge && is_version_less(mcVersion, 1, 13, 0)) {
        bool has_tweak = false;
        for (size_t i = 0; i < game_args.size(); ++i) {
            if (game_args[i] == "--tweakClass") { has_tweak = true; break; }
        }
        if (!has_tweak) {
            const std::string tweaker = is_version_less(mcVersion, 1, 8, 0)
                ? "cpw.mods.fml.common.launcher.FMLTweaker"
                : "net.minecraftforge.fml.common.launcher.FMLTweaker";
            game_args.push_back("--tweakClass");
            game_args.push_back(tweaker);
            LOG_INFO("Legacy Forge: added --tweakClass " << tweaker);
        }
    }

    std::vector<std::string> exec_args;
    fs::path argfile = argfile_dir / "java-args.argfile";
    { std::error_code mk; fs::create_directories(argfile_dir, mk); }
    bool use_argfile = (javaVersion >= 9);

    if (use_argfile) {
    std::ofstream af(argfile, std::ios::binary);
    if (!af.is_open()) {
        LOG_ERROR("Failed to create argfile: " << argfile.string());
        return false;
    }

    // Вспомогательная лямбда для вывода понятной ошибки пользователю и очистки мусора
    auto fail_with_hint = [&](const std::string& tech_reason) -> bool {
        LOG_ERROR("Critical error: " << tech_reason);
        LOG_ERROR("SOLUTION: Please move the launcher and game folder to a path");
        LOG_ERROR("containing only Latin letters and numbers (e.g., C:\\Minecraft).");
        af.close();
        std::error_code ec;
        fs::remove(argfile, ec); // Удаляем битый/недописанный файл
        return false;
    };

    // Лямбда для записи одного аргумента
    auto write_arg = [&](const std::string& arg, bool is_path) -> bool {
        bool lossy = false;
        std::string s = to_argfile_encoding(arg, lossy);
        
        if (lossy) {
            return fail_with_hint("argument does not fit into ANSI encoding: " + arg);
        }

        if (is_path) {
            std::replace(s.begin(), s.end(), '\\', '/');
        }

        af << '"';
        for (char c : s) {
            if (c == '"' || c == '\\') af << '\\';
            af << c;
        }
        af << "\"\n";
        return true;
    };

    bool ok = true;

    // 1. JVM аргументы
    for (const auto& a : jvm_args) {
        bool is_path = (a.rfind("-Djava.library.path=", 0) == 0);
        if (!write_arg(a, is_path)) {
            ok = false; 
            break; // fail_with_hint уже почистил файл
        }
    }

    // 2. Classpath
    if (ok) {
        af << "-cp ";
        ok = write_arg(classpath, true);
    }

    af.flush();
    if (!ok || !af) {
        if (ok) { // Если упало не на write_arg, а на flush/диске
            LOG_ERROR("Failed to write to argfile (possibly out of disk space).");
            af.close();
            std::error_code ec;
            fs::remove(argfile, ec);
        }
        return false;
    }
    af.close();

    // 3. Формируем путь к самому argfile (ANSI-safe через 8.3)
    std::string argfile_arg;
#ifdef _WIN32
    {
        std::wstring wpath = argfile.wstring();
        std::wstring wshort = shorten_one(wpath);
        std::string ansi_path;
        if (!wide_to_ansi(wshort, ansi_path)) {
            return fail_with_hint("argfile path does not fit into ANSI encoding: " + argfile.string());
        }
        std::replace(ansi_path.begin(), ansi_path.end(), '\\', '/');
        argfile_arg = std::move(ansi_path);
    }
#else
    argfile_arg = argfile.string();
#endif

    // Кавычки не ставим! win_quote_arg / execvp сделают это сами.
    exec_args.push_back("@" + argfile_arg);
} else {
        for (const auto& a : jvm_args) exec_args.push_back(a);
        exec_args.push_back("-cp");
        exec_args.push_back(classpath);
    }
    exec_args.push_back(mainClass);
    for (const auto& a : game_args) exec_args.push_back(a);

    LOG_INFO("=== ЗАПУСК MINECRAFT ===");
    LOG_INFO("Nick:       " << nickname);
    LOG_INFO("Version:    " << version << (isFabric ? " (Fabric MC " + mcVersion + ")" : ""));
    LOG_INFO("Memory:     " << memory_mb << " MB");
    LOG_INFO("Java:       " << java_path << " (v" << javaVersion << ")");
    LOG_INFO("MainClass:  " << mainClass);
    LOG_INFO("Classpath:  " << classpath_entries.size() << " entries");

    if (std::getenv("LAUNCHER_DEBUG") != nullptr) {
        std::string cmdline = java_path;
        for (const auto& a : exec_args) cmdline += " \"" + a + "\"";
        LOG_DEBUG("Command: " << cmdline);
    }

    LOG_INFO("Game log: " << game_log_path().string());
    int ret = run_java(fs::path(java_path), exec_args,
                       /*close_on_launch=*/!wait_for_exit);

    if (ret == -1) {
        LOG_ERROR("Не удалось создать процесс Java");
        log_game_output_tail();
        return false;
    }
    if (wait_for_exit) {
        if (ret == 0) {
            LOG_INFO("Процесс игры завершился с кодом 0");
            return true;
        }
        LOG_ERROR("Процесс игры завершился с кодом " << ret);
        log_game_output_tail();
        return false;
    }
    if (ret == 0) {
        LOG_INFO("Игра успешно запущена");
        return true;
    }
    LOG_ERROR("Игра упала сразу после старта (код " << ret << ")");
    log_game_output_tail();
    return false;
}


bool JavaLauncher::launch(const std::string& nickname_raw,
                          const std::string& version_id,
                          int memory_mb,
                          bool wait_for_exit) {
    return launch_impl(nickname_raw, version_id, version_id, memory_mb,
                       launcher_paths::minecraft_dir(),
                       launcher_paths::minecraft_dir() / "assets", {},
                       launcher_paths::minecraft_dir(), {}, wait_for_exit);
}

bool JavaLauncher::launch(const Instance& inst, bool wait_for_exit) {
    return launch_impl(inst.nickname, inst.version_id, inst.mc_version,
                       inst.memory_mb, inst.game_dir(),
                       launcher_paths::shared_assets_dir(), inst.mods_dir(),
                       inst.root_path, inst.java_path, wait_for_exit);
}

void JavaLauncher::open_url(const std::string& url) {
    LOG_INFO("Opening URL: " << url);
#ifdef _WIN32
    std::wstring wurl = utf8_to_wide(url);
    HINSTANCE r = ShellExecuteW(nullptr, L"open", wurl.c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) > 32) {
        LOG_INFO("URL opened");
    } else {
        LOG_ERROR("ShellExecuteW failed, code=" << reinterpret_cast<INT_PTR>(r));
    }
#else
    pid_t pid = 0;
    const char* argv[] = {"xdg-open", url.c_str(), nullptr};
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr,
                     const_cast<char**>(argv), environ) == 0) {
        LOG_INFO("URL opened");
    } else {
        LOG_ERROR("posix_spawnp(xdg-open) failed");
    }
#endif
}
