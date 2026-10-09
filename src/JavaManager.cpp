#include "JavaManager.h"
#include "curl_tls.h"
#include "logger.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <vector>
#include <sstream>
#include <fstream>
#include <chrono>
#include <thread>
#include <iomanip>
#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

bool JavaManager::fileExists(const fs::path& path)
{
    return fs::exists(path) && fs::is_regular_file(path);
}

static std::string execCommandFirstLine(const std::string& cmd)
{
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    char buffer[1024];
    std::string result;
    if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result = buffer;
        if (!result.empty() && result.back() == '\n') result.pop_back();
    }
    pclose(pipe);
    return result;
}

// Загрузка файла встроенным libcurl — не зависим от системного curl
// (на Windows это снимает требование к наличию curl.exe в PATH).
static size_t http_write_cb(void* ptr, size_t size, size_t nmemb, FILE* file)
{
    return fwrite(ptr, size, nmemb, file);
}

static bool http_download_file(const std::string& url, const fs::path& path)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

#ifdef _WIN32
    FILE* file = _wfopen(path.c_str(), L"wb");
#else
    FILE* file = fopen(path.c_str(), "wb");
#endif
    if (!file) return false;

    CURL* curl = curl_easy_init();
    bool ok = false;
    if (curl) {
        curl_tls::apply(curl);  // NATIVE_CA / cacert.pem / системные CA
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Minecraft-Launcher/1.0");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        CURLcode rc = curl_easy_perform(curl);
        ok = (rc == CURLE_OK);
        if (!ok)
            LOG_ERROR("Java download failed: " << curl_easy_strerror(rc) << " URL: " << url);
        curl_easy_cleanup(curl);
    }
    fclose(file);
    if (!ok) fs::remove(path, ec);
    return ok;
}

void JavaManager::log(const std::string& message, bool isError)
{

    try {
        std::ofstream logFile(logPath, std::ios::app);
        if (logFile.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            logFile << "[" << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S") << "] "
                    << (isError ? "[ERROR] " : "[INFO] ")
                    << message << "\n";
        }
    } catch (...) {}


    if (isError) {
        std::cerr << message << "\n";
    } else {
        std::cout << message << "\n";
    }
}

static fs::path clauncher_home()
{
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (home && *home) return fs::path(home) / ".clauncher";
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive && path) return fs::path(std::string(drive) + path) / ".clauncher";
    return fs::path(".clauncher");
#else
    const char* home = std::getenv("HOME");
    if (home && *home) return fs::path(home) / ".clauncher";
    return fs::path(".clauncher");
#endif
}

std::string JavaManager::shellQuote(const std::string& value)
{
#ifdef _WIN32
    std::string result = "\"";
    for (char c : value) {
        if (c == '"') result += "\\\"";
        else if (c == '\\') result += "\\\\";
        else result += c;
    }
    return result + "\"";
#else
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') result += "'\\''";
        else result += c;
    }
    return result + "'";
#endif
}

int JavaManager::parseVersionPart(const std::string& version, std::size_t& pos)
{
    while (pos < version.size() && version[pos] == '.') ++pos;
    if (pos >= version.size() || !std::isdigit(version[pos])) return 0;
    int val = 0;
    while (pos < version.size() && std::isdigit(version[pos]))
        val = val * 10 + (version[pos++] - '0');
    return val;
}

JavaManager::JavaVersion JavaManager::getRequiredJava(const std::string& versionId) const
{
    std::string mcVersion = versionId;


    if (versionId.find("fabric-loader-") == 0) {
        size_t lastDash = versionId.rfind('-');
        if (lastDash != std::string::npos) {
            mcVersion = versionId.substr(lastDash + 1);
        } else {
            throw std::runtime_error("Unsupported Fabric version format: " + versionId);
        }
    }

    if (mcVersion.empty()) throw std::invalid_argument("Empty Minecraft version");

    std::size_t pos = 0;
    int major = parseVersionPart(mcVersion, pos);

    if (major == 1) {
        int minor = parseVersionPart(mcVersion, pos);
        if (minor < 17) return JavaVersion::Java8;
        if (minor < 20) return JavaVersion::Java17;
        if (minor == 20) {
            int patch = parseVersionPart(mcVersion, pos);
            return (patch >= 5) ? JavaVersion::Java21 : JavaVersion::Java17;
        }
        return JavaVersion::Java21;
    }

    // Ветка 26.x и новее отфильтрована в VersionManager. Это предохранитель
    // на случай ручного вызова с неподдерживаемой версией.
    throw std::runtime_error(
        "Unsupported Minecraft version (only the 1.x line is supported): " + mcVersion);
}

std::string JavaManager::getDownloadUrl(JavaVersion version)
{
#ifdef _WIN32
    const char* os = "windows";
#else
    const char* os = "linux";
#endif

    const char* versions[] = {"8", "17", "21"};
    int idx = static_cast<int>(version);




    return std::string("https://api.adoptium.net/v3/binary/latest/") +
           versions[idx] + "/ga/" + os + "/x64/jre/hotspot/normal/eclipse?project=jdk";
}

fs::path JavaManager::getJavaDirectory(JavaVersion version) const
{
    const char* dirs[] = {"java8", "java17", "java21"};
    return runtimeDir / dirs[static_cast<int>(version)];
}

fs::path JavaManager::getJavaBinary(JavaVersion version) const
{
    fs::path dir = getJavaDirectory(version);
#ifdef _WIN32
    return dir / "bin" / "java.exe";
#else
    return dir / "bin" / "java";
#endif
}

void JavaManager::checkDependencies()
{
#ifdef _WIN32
    // tar.exe (bsdtar) встроен в Windows 10 1803+ и нужен для распаковки JRE
    // и нативных библиотек (jar = zip). PowerShell остаётся лишь запасным
    // вариантом распаковки Java.
    if (std::system("tar --version >NUL 2>&1") != 0) {
        throw std::runtime_error(
            "tar.exe not found. Windows 10 1803 or newer is required "
            "(or install 7-Zip / bsdtar manually).");
    }
#else
    if (std::system("command -v curl >/dev/null 2>&1") != 0) {
        throw std::runtime_error("curl is not installed. Please install curl.");
    }
    if (std::system("command -v tar >/dev/null 2>&1") != 0) {
        throw std::runtime_error("tar is not installed. Please install tar.");
    }
    // unzip нужен MinecraftInstaller для распаковки нативных LWJGL-библиотек.
    if (std::system("command -v unzip >/dev/null 2>&1") != 0) {
        throw std::runtime_error(
            "unzip is not installed (needed to extract Minecraft native libraries). "
            "Please install unzip.");
    }
#endif
}

int JavaManager::extractMajorJavaVersion(const std::string& versionOutput)
{
    size_t pos = versionOutput.find("version \"");
    if (pos == std::string::npos) {
        pos = versionOutput.find("version ");
        if (pos == std::string::npos) return -1;
        pos += 8;
    } else {
        pos += 9;
    }

    while (pos < versionOutput.size() && (versionOutput[pos] == '"' || versionOutput[pos] == ' '))
        ++pos;

    std::string verStr;
    while (pos < versionOutput.size() && (std::isdigit(versionOutput[pos]) || versionOutput[pos] == '.'))
        verStr += versionOutput[pos++];

    if (verStr.empty()) return -1;

    if (verStr.size() >= 2 && verStr[0] == '1' && verStr[1] == '.') {
        std::string rest = verStr.substr(2);
        size_t dot = rest.find('.');
        if (dot != std::string::npos) rest = rest.substr(0, dot);
        try {
            return std::stoi(rest);
        } catch (...) {
            return -1;
        }
    }

    size_t dot = verStr.find('.');
    if (dot != std::string::npos) verStr = verStr.substr(0, dot);
    try {
        return std::stoi(verStr);
    } catch (...) {
        return -1;
    }
}

JavaManager::JavaVersion JavaManager::javaVersionFromMajor(int major) const
{
    switch (major) {
        case 8:  return JavaVersion::Java8;
        case 17: return JavaVersion::Java17;
        case 21: return JavaVersion::Java21;
        default:
            throw std::runtime_error("Unsupported Java major: " + std::to_string(major));
    }
}

int JavaManager::requiredMajor(const std::string& mcVersion) const
{
    switch (getRequiredJava(mcVersion)) {
        case JavaVersion::Java8:  return 8;
        case JavaVersion::Java17: return 17;
        case JavaVersion::Java21: return 21;
    }
    throw std::runtime_error("Unsupported Java requirement");
}

void JavaManager::verifyJavaInstallation(const fs::path& javaBinary, JavaVersion expectedVersion)
{
    if (!fileExists(javaBinary)) {
        throw std::runtime_error("Java binary not found: " + javaBinary.string());
    }


    std::string cmd = shellQuote(javaBinary.string()) + " -version 2>&1";
    std::string out = execCommandFirstLine(cmd);

    if (out.empty()) {
        throw std::runtime_error("Java binary does not respond: " + javaBinary.string());
    }

    int ver = extractMajorJavaVersion(out);
    if (ver < 0) {
        throw std::runtime_error("Cannot determine Java version from output: " + out);
    }

    int needVer = 0;
    switch (expectedVersion) {
        case JavaVersion::Java8:  needVer = 8; break;
        case JavaVersion::Java17: needVer = 17; break;
        case JavaVersion::Java21: needVer = 21; break;
    }

    if (ver != needVer) {
        throw std::runtime_error("Java version mismatch. Expected " + std::to_string(needVer) +
                                ", got " + std::to_string(ver));
    }

    log("Java verification passed: version " + std::to_string(ver));
}

void JavaManager::downloadJava(JavaVersion version)
{
    const fs::path javaDir = getJavaDirectory(version);
    const fs::path archivePath = runtimeDir / (javaDir.filename().string() +
#ifdef _WIN32
        ".zip"
#else
        ".tar.gz"
#endif
    );

    const std::string url = getDownloadUrl(version);

    log("Downloading Java from: " + url);
    log("Saving to: " + archivePath.string());

    std::error_code ec;
    fs::create_directories(javaDir, ec);
    if (ec) throw std::runtime_error("Failed to create " + javaDir.string());

    if (fs::exists(archivePath)) {
        fs::remove(archivePath, ec);
    }

    const int MAX_RETRIES = 3;

    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        log("Download attempt " + std::to_string(attempt) + " of " + std::to_string(MAX_RETRIES));

        bool downloaded = http_download_file(url, archivePath);

        if (downloaded && fileExists(archivePath)) {
            uintmax_t size = fs::file_size(archivePath);
            if (size > 0) {
                log("Download complete: " + std::to_string(size / (1024*1024)) + " MB");
                extractJava(version, archivePath);
                fs::remove(archivePath, ec);
                return;
            }
        }


        fs::remove(archivePath, ec);

        if (attempt < MAX_RETRIES) {
            int delay = 5 * attempt;
            log("Attempt " + std::to_string(attempt) + " failed. Retrying in " +
                std::to_string(delay) + " seconds...", true);
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        } else {

            throw std::runtime_error("Failed to download Java after " +
                                    std::to_string(MAX_RETRIES) + " attempts. " +
                                    "Check your internet connection.");
        }
    }


    throw std::runtime_error("Unexpected: download loop ended without result");
}

void JavaManager::extractJava(JavaVersion version, const fs::path& archivePath)
{
    const fs::path javaDir = getJavaDirectory(version);
    std::error_code ec;

    fs::remove_all(javaDir, ec);
    fs::create_directories(javaDir, ec);

    log("Extracting Java to: " + javaDir.string());

    std::string cmd;
#ifdef _WIN32
    // Основной путь — встроенный в Windows 10 1803+ bsdtar (умеет zip).
    // PowerShell Expand-Archive остаётся запасным вариантом на старых системах.
    cmd = "tar -xf " + shellQuote(archivePath.string()) +
          " -C " + shellQuote(javaDir.string());
    int result = std::system(cmd.c_str());
    if (result != 0) {
        log("tar extraction failed (code " + std::to_string(result) +
            "), falling back to PowerShell Expand-Archive", true);
        cmd = "powershell -NoProfile -Command \"$ProgressPreference='SilentlyContinue'; "
              "Expand-Archive -Path " + shellQuote(archivePath.string()) +
              " -DestinationPath " + shellQuote(javaDir.string()) + " -Force\"";
        result = std::system(cmd.c_str());
    }
#else
    cmd = "tar -xzf " + shellQuote(archivePath.string()) +
          " -C " + shellQuote(javaDir.string()) + " --strip-components=1";
    int result = std::system(cmd.c_str());
#endif

    if (result != 0) {
        fs::remove_all(javaDir, ec);
        throw std::runtime_error("Extraction failed (exit code " + std::to_string(result) + ")");
    }

#ifdef _WIN32

    std::vector<fs::path> subdirs;
    try {
        for (const auto& entry : fs::directory_iterator(javaDir)) {
            if (entry.is_directory()) {
                subdirs.push_back(entry.path());
            }
        }
    } catch (const fs::filesystem_error& e) {
        throw std::runtime_error("Failed to list extracted directory: " + std::string(e.what()));
    }

    if (subdirs.size() == 1) {
        fs::path src = subdirs[0];
        std::error_code ec2;

        try {
            for (const auto& sub : fs::directory_iterator(src)) {
                fs::path dest = javaDir / sub.path().filename();
                fs::rename(sub.path(), dest, ec2);
                if (ec2) {
                    throw std::runtime_error("Failed to move " + sub.path().filename().string() +
                                           ": " + ec2.message());
                }
            }
        } catch (const fs::filesystem_error& e) {
            throw std::runtime_error("Error during Java extraction restructuring: " +
                                    std::string(e.what()));
        }

        fs::remove(src, ec2);
    }
#endif

    const fs::path javaBinary = getJavaBinary(version);
    if (!fileExists(javaBinary)) {
        fs::remove_all(javaDir, ec);
        throw std::runtime_error("Java binary not found after extraction: " + javaBinary.string());
    }

#ifndef _WIN32
    makeExecutable(javaBinary);
#endif


    verifyJavaInstallation(javaBinary, version);

    log("Extraction complete.");
}

void JavaManager::makeExecutable(const fs::path& javaBinary)
{
#ifndef _WIN32
    std::error_code ec;
    fs::permissions(javaBinary,
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
        fs::perm_options::add, ec);
    if (ec) {
        throw std::runtime_error("Failed to set executable permission on " + javaBinary.string());
    }
#endif
}

JavaManager::JavaManager()
{
    fs::path base = clauncher_home();
    runtimeDir = base / "runtime";
    logPath = base / "java-manager.log";

    std::error_code ec;
    fs::create_directories(runtimeDir, ec);
    if (ec) {
        throw std::runtime_error("Failed to create runtime directory: " + runtimeDir.string());
    }

    log("JavaManager initialized. Runtime dir: " + runtimeDir.string());
}

fs::path JavaManager::ensureJava(int major)
{
    try {
        checkDependencies();
        const JavaVersion required = javaVersionFromMajor(major);
        const fs::path javaBinary = getJavaBinary(required);
        log("Checking managed Java " + std::to_string(major) + ": " + javaBinary.string());

        if (fileExists(javaBinary)) {
            log("Using cached portable Java: " + javaBinary.string());
            return javaBinary;
        }

        log("Downloading portable Java...");
        downloadJava(required);

        if (!fileExists(javaBinary)) {
            throw std::runtime_error("Unable to obtain Java " + std::to_string(major));
        }

        log("Using portable Java: " + javaBinary.string());
        return javaBinary;
    } catch (const std::exception& e) {
        log("JavaManager error: " + std::string(e.what()), true);
        throw;
    }
}

fs::path JavaManager::resolveJava(const JavaInstance& instance)
{
    if (!instance.manual_path.empty()) {
        const fs::path& p = instance.manual_path;
        if (fs::is_directory(p)) {
#ifdef _WIN32
            fs::path bin = p / "bin" / "java.exe";
            if (!fileExists(bin)) bin = p / "java.exe";
#else
            fs::path bin = p / "bin" / "java";
            if (!fileExists(bin)) bin = p / "java";
#endif
            if (fileExists(bin)) return bin;
        } else if (fileExists(p)) {
            return p;
        }
        throw std::runtime_error("Manual Java path not found: " + p.string());
    }
    return ensureJava(instance.major);
}
