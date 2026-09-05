#include "JavaManager.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <vector>
#include <map>
#include <sstream>
#include <fstream>
#include <chrono>
#include <thread>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==========

static std::string getHomeDir()
{
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) return std::string(appdata);
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) return std::string(userprofile) + "\\AppData\\Roaming";
    return ".";
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) : ".";
#endif
}

static std::string getLauncherBaseDir()
{
#ifdef _WIN32
    return getHomeDir() + "\\AppData\\Local\\minecraft-launcher";
#else
    return getHomeDir() + "/.minecraft-launcher";
#endif
}

static std::string getRuntimeDir()
{
    return getLauncherBaseDir() + "/runtime";
}

bool JavaManager::fileExists(const fs::path& path)
{
    return fs::exists(path) && fs::is_regular_file(path);
}

// Выполняет команду и возвращает первую строку вывода (или пустую строку при ошибке)
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

// ========== ЛОГИРОВАНИЕ ==========

void JavaManager::log(const std::string& message, bool isError)
{
    // 1. Пишем в файл
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
    
    // 2. Пишем в консоль
    if (isError) {
        std::cerr << message << "\n";
    } else {
        std::cout << message << "\n";
    }
}

// ========== ОПРЕДЕЛЕНИЕ ДИСТРИБУТИВА (LINUX) ==========

struct DistroInfo {
    std::string name;
    std::string pkgMgr;
    std::string installCmdTemplate;
};

static DistroInfo detectDistro()
{
#ifdef _WIN32
    return {"windows", "", ""};
#else
    std::ifstream file("/etc/os-release");
    if (!file.is_open()) {
        file.open("/usr/lib/os-release");
        if (!file.is_open()) {
            return {"unknown", "", ""};
        }
    }
    
    std::map<std::string, std::string> vars;
    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            if (!value.empty() && value.front() == '"') value = value.substr(1);
            if (!value.empty() && value.back() == '"') value.pop_back();
            vars[key] = value;
        }
    }
    
    std::string id = vars["ID"];
    std::string idLike = vars["ID_LIKE"];
    
    DistroInfo distro;
    
    if (id == "ubuntu" || id == "debian" || id == "linuxmint" || 
        idLike.find("debian") != std::string::npos) {
        distro.name = id;
        distro.pkgMgr = "apt";
        distro.installCmdTemplate = "sudo apt update && sudo apt install -y {package}";
    }
    else if (id == "fedora" || id == "rhel" || id == "centos" ||
             idLike.find("fedora") != std::string::npos) {
        distro.name = id;
        distro.pkgMgr = "dnf";
        distro.installCmdTemplate = "sudo dnf install -y {package}";
    }
    else if (id == "arch" || id == "manjaro" ||
             idLike.find("arch") != std::string::npos) {
        distro.name = id;
        distro.pkgMgr = "pacman";
        distro.installCmdTemplate = "sudo pacman -S --noconfirm {package}";
    }
    else if (id == "opensuse" || id == "opensuse-leap" || id == "opensuse-tumbleweed") {
        distro.name = id;
        distro.pkgMgr = "zypper";
        distro.installCmdTemplate = "sudo zypper install -y {package}";
    }
    else if (id == "alpine") {
        distro.name = id;
        distro.pkgMgr = "apk";
        distro.installCmdTemplate = "sudo apk add {package}";
    }
    else {
        distro.name = id.empty() ? "unknown" : id;
        distro.pkgMgr = "";
        distro.installCmdTemplate = "";
    }
    
    return distro;
#endif
}

// ========== shellQuote ==========

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

// ========== ПАРСИНГ ВЕРСИИ ==========

int JavaManager::parseVersionPart(const std::string& version, std::size_t& pos)
{
    while (pos < version.size() && version[pos] == '.') ++pos;
    if (pos >= version.size() || !std::isdigit(version[pos])) return 0;
    int val = 0;
    while (pos < version.size() && std::isdigit(version[pos]))
        val = val * 10 + (version[pos++] - '0');
    return val;
}

// Исправленная версия: поддерживает имена вида "fabric-loader-0.15.11-1.19.4"
JavaManager::JavaVersion JavaManager::getRequiredJava(const std::string& versionId) const
{
    std::string mcVersion = versionId;

    // Если это Fabric, извлекаем версию Minecraft (часть после последнего '-')
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
    
    if (major >= 26) return JavaVersion::Java21;
    
    throw std::runtime_error("Unsupported Minecraft version: " + mcVersion);
}

// ========== URL ДЛЯ СКАЧИВАНИЯ ==========

std::string JavaManager::getDownloadUrl(JavaVersion version)
{
#ifdef _WIN32
    const char* os = "windows";
#else
    const char* os = "linux";
#endif
    
    const char* versions[] = {"8", "17", "21"};
    int idx = static_cast<int>(version);
    
    // ИСПРАВЛЕНО: /jre/ вместо /jdk/ — для игры JRE достаточна, архив меньше
    // примерно в 4 раза (Java 21: ~190 МБ JDK против ~45 МБ JRE).
    // Для слабого ПК и медленного канала это минуты ожидания.
    return std::string("https://api.adoptium.net/v3/binary/latest/") +
           versions[idx] + "/ga/" + os + "/x64/jre/hotspot/normal/eclipse?project=jdk";
}

// ========== ПУТИ ==========

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

// ========== ПРОВЕРКА ЗАВИСИМОСТЕЙ ==========

void JavaManager::checkDependencies()
{
#ifdef _WIN32
    std::string testCmd = "powershell -NoProfile -Command \"exit\"";
    if (std::system(testCmd.c_str()) != 0) {
        throw std::runtime_error("PowerShell is not available. Please install PowerShell 5.0+.");
    }
#else
    if (std::system("command -v curl >/dev/null 2>&1") != 0) {
        throw std::runtime_error("curl is not installed. Please install curl.");
    }
    if (std::system("command -v tar >/dev/null 2>&1") != 0) {
        throw std::runtime_error("tar is not installed. Please install tar.");
    }
#endif
}

// ========== ИЗВЛЕЧЕНИЕ ОСНОВНОЙ ВЕРСИИ JAVA ==========

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

// ========== ПОИСК СИСТЕМНОЙ JAVA ==========

bool JavaManager::findSystemJava(fs::path& foundPath, JavaVersion required) const
{
#ifdef _WIN32
    return false;
#else
    // ИСПРАВЛЕНО: раньше требовалось СТРОГОЕ совпадение версии. Из-за этого
    // системная Java 21 не считалась пригодной для версий MC, требующих Java 17,
    // (и наоборот не было бы проблемой, но 22+ для 21 — реальный случай).
    // Современные версии MC (требующие Java 17+) прекрасно работают и на более
    // новых JVM, поэтому для needVer >= 17 принимаем sysVer >= needVer.
    // Для Java 8 строгость сохранена: старые версии MC чувствительны к JVM.
    auto versionMatches = [](int sysVer, int needVer) {
        return (sysVer == needVer) || (needVer >= 17 && sysVer > needVer);
    };

    std::string versionOutput = execCommandFirstLine("java -version 2>&1");
    if (!versionOutput.empty()) {
        int sysVer = extractMajorJavaVersion(versionOutput);
        int needVer = 0;
        switch (required) {
            case JavaVersion::Java8:  needVer = 8; break;
            case JavaVersion::Java17: needVer = 17; break;
            case JavaVersion::Java21: needVer = 21; break;
        }
        if (versionMatches(sysVer, needVer)) {
            foundPath = "/usr/bin/java";
            return true;
        }
    }
    
    const std::vector<std::string> searchPaths = {
        "/usr/bin/java",
        "/usr/local/bin/java",
        "/opt/java/bin/java",
        "/snap/bin/java"
    };
    
    for (const auto& path : searchPaths) {
        if (fileExists(path)) {
            std::string cmd = path + " -version 2>&1";
            std::string out = execCommandFirstLine(cmd);
            if (!out.empty()) {
                int sysVer = extractMajorJavaVersion(out);
                int needVer = 0;
                switch (required) {
                    case JavaVersion::Java8:  needVer = 8; break;
                    case JavaVersion::Java17: needVer = 17; break;
                    case JavaVersion::Java21: needVer = 21; break;
                }
                if (versionMatches(sysVer, needVer)) {
                    foundPath = path;
                    return true;
                }
            }
        }
    }
    
    return false;
#endif
}

// ========== УСТАНОВКА ЧЕРЕЗ ПАКЕТНЫЙ МЕНЕДЖЕР ==========

void JavaManager::installSystemJava(JavaVersion required)
{
#ifndef _WIN32
    DistroInfo distro = detectDistro();
    
    if (distro.pkgMgr.empty()) {
        throw std::runtime_error("Unsupported distribution: " + distro.name + 
                                ". Please install Java manually.");
    }
    
    std::map<JavaVersion, std::string> packages;
    if (distro.pkgMgr == "apt") {
        packages[JavaVersion::Java8]  = "openjdk-8-jre-headless";
        packages[JavaVersion::Java17] = "openjdk-17-jre-headless";
        packages[JavaVersion::Java21] = "openjdk-21-jre-headless";
    } else if (distro.pkgMgr == "dnf") {
        packages[JavaVersion::Java8]  = "java-1.8.0-openjdk-headless";
        packages[JavaVersion::Java17] = "java-17-openjdk-headless";
        packages[JavaVersion::Java21] = "java-21-openjdk-headless";
    } else if (distro.pkgMgr == "pacman") {
        packages[JavaVersion::Java8]  = "jdk8-openjdk";
        packages[JavaVersion::Java17] = "jdk17-openjdk";
        packages[JavaVersion::Java21] = "jdk21-openjdk";
    } else if (distro.pkgMgr == "zypper") {
        packages[JavaVersion::Java8]  = "java-1_8_0-openjdk-headless";
        packages[JavaVersion::Java17] = "java-17-openjdk-headless";
        packages[JavaVersion::Java21] = "java-21-openjdk-headless";
    } else if (distro.pkgMgr == "apk") {
        packages[JavaVersion::Java8]  = "openjdk8-jre";
        packages[JavaVersion::Java17] = "openjdk17-jre";
        packages[JavaVersion::Java21] = "openjdk21-jre";
    } else {
        throw std::runtime_error("Unsupported package manager: " + distro.pkgMgr);
    }
    
    if (packages.find(required) == packages.end()) {
        throw std::runtime_error("No package mapping for this Java version");
    }
    
    const std::string& pkgName = packages[required];
    
    log("Installing " + pkgName + " via " + distro.pkgMgr);
    
    std::string installCmd = distro.installCmdTemplate;
    size_t pos = installCmd.find("{package}");
    if (pos != std::string::npos) {
        installCmd.replace(pos, 9, pkgName);
    }
    
    int result = std::system(installCmd.c_str());
    if (result != 0) {
        throw std::runtime_error("Package manager installation failed (exit code " + std::to_string(result) + ")");
    }
    
    log("Java installation via " + distro.pkgMgr + " complete.");
#endif
}

// ========== ПРОВЕРКА УСТАНОВЛЕННОЙ JAVA ==========

void JavaManager::verifyJavaInstallation(const fs::path& javaBinary, JavaVersion expectedVersion)
{
    if (!fileExists(javaBinary)) {
        throw std::runtime_error("Java binary not found: " + javaBinary.string());
    }
    
    // Проверяем, что Java работает и версия совпадает
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

// ========== СКАЧИВАНИЕ ПОРТАТИВНОЙ JAVA (ИСПРАВЛЕННЫЙ LOOP) ==========

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
        
        std::string cmd = "curl -L --progress-bar --fail --connect-timeout 30 --max-time 600 " +
                          shellQuote(url) + " -o " + shellQuote(archivePath.string());
        
        int result = std::system(cmd.c_str());
        
        // Проверяем успех
        if (result == 0 && fileExists(archivePath)) {
            uintmax_t size = fs::file_size(archivePath);
            if (size > 0) {
                log("Download complete: " + std::to_string(size / (1024*1024)) + " MB");
                extractJava(version, archivePath);
                fs::remove(archivePath, ec);
                return;  // ✅ ЯВНЫЙ ВЫХОД ПРИ УСПЕХЕ
            }
        }
        
        // ❌ Ошибка - удаляем файл
        fs::remove(archivePath, ec);
        
        if (attempt < MAX_RETRIES) {
            int delay = 5 * attempt;
            log("Attempt " + std::to_string(attempt) + " failed. Retrying in " + 
                std::to_string(delay) + " seconds...", true);
            std::this_thread::sleep_for(std::chrono::seconds(delay));
        } else {
            // Все попытки исчерпаны
            throw std::runtime_error("Failed to download Java after " + 
                                    std::to_string(MAX_RETRIES) + " attempts. " +
                                    "Check your internet connection.");
        }
    }
    
    // Достигнуть этого места невозможно, но добавляем защиту
    throw std::runtime_error("Unexpected: download loop ended without result");
}

// ========== РАСПАКОВКА ==========

void JavaManager::extractJava(JavaVersion version, const fs::path& archivePath)
{
    const fs::path javaDir = getJavaDirectory(version);
    std::error_code ec;
    
    fs::remove_all(javaDir, ec);
    fs::create_directories(javaDir, ec);
    
    log("Extracting Java to: " + javaDir.string());
    
    std::string cmd;
#ifdef _WIN32
    cmd = "powershell -NoProfile -Command \"$ProgressPreference='SilentlyContinue'; "
          "Expand-Archive -Path " + shellQuote(archivePath.string()) +
          " -DestinationPath " + shellQuote(javaDir.string()) + " -Force\"";
#else
    cmd = "tar -xzf " + shellQuote(archivePath.string()) +
          " -C " + shellQuote(javaDir.string()) + " --strip-components=1";
#endif
    
    int result = std::system(cmd.c_str());
    if (result != 0) {
        fs::remove_all(javaDir, ec);
        throw std::runtime_error("Extraction failed (exit code " + std::to_string(result) + ")");
    }
    
#ifdef _WIN32
    // На Windows перемещаем содержимое из единственной подпапки
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
    
    // Проверяем работоспособность Java
    verifyJavaInstallation(javaBinary, version);
    
    log("Extraction complete.");
}

// ========== УСТАНОВКА ПРАВ НА ВЫПОЛНЕНИЕ ==========

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

// ========== КОНСТРУКТОР ==========

JavaManager::JavaManager()
{
    std::string base = getLauncherBaseDir();
    runtimeDir = fs::path(base) / "runtime";
    logPath = fs::path(base) / "java-manager.log";
    
    std::error_code ec;
    fs::create_directories(runtimeDir, ec);
    if (ec) {
        throw std::runtime_error("Failed to create runtime directory: " + runtimeDir.string());
    }
    
    log("JavaManager initialized. Runtime dir: " + runtimeDir.string());
}

// ========== ОСНОВНОЙ МЕТОД ==========

fs::path JavaManager::ensureJava(const std::string& mcVersion)
{
    try {
        checkDependencies();
        log("Checking Java for Minecraft version: " + mcVersion);
        
        const JavaVersion required = getRequiredJava(mcVersion);
        const fs::path javaBinary = getJavaBinary(required);
        
        const char* versionNames[] = {"Java 8", "Java 17", "Java 21"};
        log("Required: " + std::string(versionNames[static_cast<int>(required)]));
        
#ifdef _WIN32
        // Windows: всегда портативная
        if (fileExists(javaBinary)) {
            log("Using cached portable Java: " + javaBinary.string());
            return javaBinary;
        }
        
        log("Downloading portable Java for Windows...");
        downloadJava(required);
        
        if (!fileExists(javaBinary)) {
            throw std::runtime_error("Unable to obtain Java for Windows.");
        }
        
        log("Java ready: " + javaBinary.string());
        return javaBinary;
        
#else
        // Linux
        fs::path systemJavaPath;
        if (findSystemJava(systemJavaPath, required)) {
            log("Using system Java: " + systemJavaPath.string());
            return systemJavaPath;
        }
        
        if (fileExists(javaBinary)) {
            log("Using cached portable Java: " + javaBinary.string());
            return javaBinary;
        }
        
        try {
            DistroInfo distro = detectDistro();
            log("Distribution detected: " + (distro.name.empty() ? "unknown" : distro.name));
            
            if (!distro.pkgMgr.empty()) {
                try {
                    installSystemJava(required);
                    if (findSystemJava(systemJavaPath, required)) {
                        log("Java installed via " + distro.pkgMgr + ": " + systemJavaPath.string());
                        return systemJavaPath;
                    }
                } catch (const std::exception& e) {
                    log("Package manager installation failed: " + std::string(e.what()), true);
                    log("Falling back to portable Java download.");
                }
            }
        } catch (const std::exception& e) {
            log("Distribution detection failed: " + std::string(e.what()), true);
            log("Falling back to portable Java download.");
        }
        
        log("Downloading portable Java...");
        downloadJava(required);
        
        if (!fileExists(javaBinary)) {
            throw std::runtime_error("Unable to obtain Java.");
        }
        
        log("Using portable Java: " + javaBinary.string());
        return javaBinary;
#endif
    } catch (const std::exception& e) {
        log("JavaManager error: " + std::string(e.what()), true);
        throw;
    }
}
