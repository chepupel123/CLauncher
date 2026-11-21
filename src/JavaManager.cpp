#include "JavaManager.h"
#include "paths.h"
#include "curl_tls.h"
#include "logger.h"
#include "miniz.h"
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <vector>
#include <chrono>
#include <thread>
#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

bool JavaManager::fileExists(const fs::path& path)
{
    std::error_code ec;
    return fs::exists(path, ec) && fs::is_regular_file(path, ec);
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

static size_t http_write_cb(void* ptr, size_t size, size_t nmemb, FILE* file)
{
    return fwrite(ptr, size, nmemb, file);
}

static bool http_download_file(const std::string& url, const fs::path& path)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        LOG_ERROR("Failed to create download directory: " << ec.message());
        return false;
    }

#ifdef _WIN32
    FILE* file = _wfopen(path.wstring().c_str(), L"wb");
#else
    FILE* file = fopen(path.string().c_str(), "wb");
#endif
    if (!file) {
        LOG_ERROR("Failed to open destination file for writing: " << path.string());
        return false;
    }

    CURL* curl = curl_easy_init();
    bool ok = false;
    if (curl) {
        curl_tls::apply(curl);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "CLauncher/1.0.3");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        CURLcode rc = curl_easy_perform(curl);
        ok = (rc == CURLE_OK);
        if (!ok) {
            LOG_ERROR("Java download failed: " << curl_easy_strerror(rc) << " URL: " << url);
        }
        curl_easy_cleanup(curl);
    }

    fclose(file);
    if (!ok) {
        fs::remove(path, ec);
    }
    return ok;
}

static bool isSafeRelativeEntry(const std::string& rawName, fs::path& outRelPath)
{
    if (rawName.empty()) return false;

    std::string normalized = rawName;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }

    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    if (normalized.empty()) return false;

    fs::path rel(normalized);
    for (const auto& part : rel) {
        if (part == "..") {
            return false;
        }
    }

    outRelPath = rel;
    return true;
}

static bool extractZipWithMiniz(const fs::path& archivePath, const fs::path& destDir, std::string& errOut)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));

    const std::string archiveUtf8 = archivePath.string();
    if (!mz_zip_reader_init_file(&zip, archiveUtf8.c_str(), 0)) {
        errOut = "mz_zip_reader_init_file failed for " + archiveUtf8;
        return false;
    }

    const mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    std::error_code ec;

    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            errOut = "Failed to read file stat at index " + std::to_string(i);
            mz_zip_reader_end(&zip);
            return false;
        }

        fs::path relPath;
        if (!isSafeRelativeEntry(stat.m_filename, relPath)) {
            errOut = std::string("Unsafe entry path in archive: ") + stat.m_filename;
            mz_zip_reader_end(&zip);
            return false;
        }

        const fs::path outPath = destDir / relPath;
        const bool isDir = mz_zip_reader_is_file_a_directory(&zip, i) != 0;

        if (isDir) {
            fs::create_directories(outPath, ec);
            if (ec) {
                errOut = "Failed to create directory " + outPath.string() + ": " + ec.message();
                mz_zip_reader_end(&zip);
                return false;
            }
            continue;
        }

        fs::create_directories(outPath.parent_path(), ec);
        if (ec) {
            errOut = "Failed to create parent directory for " + outPath.string() + ": " + ec.message();
            mz_zip_reader_end(&zip);
            return false;
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, outPath.string().c_str(), 0)) {
            errOut = "Failed to extract entry: " + relPath.string();
            mz_zip_reader_end(&zip);
            return false;
        }

#ifndef _WIN32
        const uint32_t unixMode = (stat.m_external_attr >> 16) & 07777;
        if (unixMode != 0) {
            fs::permissions(outPath, static_cast<fs::perms>(unixMode), fs::perm_options::replace, ec);
        }
#endif
    }

    mz_zip_reader_end(&zip);
    return true;
}

void JavaManager::log(const std::string& message, bool isError)
{
    if (isError) LOG_ERROR(message);
    else         LOG_INFO(message);
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
    if (pos >= version.size() || !std::isdigit(static_cast<unsigned char>(version[pos]))) return 0;
    int val = 0;
    while (pos < version.size() && std::isdigit(static_cast<unsigned char>(version[pos]))) {
        val = val * 10 + (version[pos++] - '0');
    }
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

    if (mcVersion.empty()) {
        throw std::invalid_argument("Empty Minecraft version");
    }

    std::size_t pos = 0;
    int major = parseVersionPart(mcVersion, pos);

    if (major >= 26) return JavaVersion::Java25;

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

    throw std::runtime_error(
        "Unsupported Minecraft version (expected the 1.x or 26.x+ line): " + mcVersion);
}

std::string JavaManager::getDownloadUrl(JavaVersion version)
{
#ifdef _WIN32
    const char* os = "windows";
#else
    const char* os = "linux";
#endif

    const char* versions[] = {"8", "17", "21", "25"};
    int idx = static_cast<int>(version);

    return std::string("https://api.adoptium.net/v3/binary/latest/") +
           versions[idx] + "/ga/" + os +
           "/x64/jre/hotspot/normal/eclipse?project=jdk&archive_type=zip";
}

fs::path JavaManager::getJavaDirectory(JavaVersion version) const
{
    const char* dirs[] = {"java8", "java17", "java21", "java25"};
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
    if (runtimeDir.empty()) {
        throw std::runtime_error("Runtime directory is not configured.");
    }

    std::error_code ec;
    fs::create_directories(runtimeDir, ec);
    if (ec) {
        throw std::runtime_error("Runtime directory is not accessible: " +
                                 runtimeDir.string() + " (" + ec.message() + ")");
    }

    const fs::path probeFile = runtimeDir / ".write_probe.tmp";
#ifdef _WIN32
    FILE* f = _wfopen(probeFile.wstring().c_str(), L"wb");
#else
    FILE* f = fopen(probeFile.string().c_str(), "wb");
#endif
    if (!f) {
        throw std::runtime_error("Runtime directory is not writable: " + runtimeDir.string());
    }
    fclose(f);
    fs::remove(probeFile, ec);
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

    while (pos < versionOutput.size() && (versionOutput[pos] == '"' || versionOutput[pos] == ' ')) {
        ++pos;
    }

    std::string verStr;
    while (pos < versionOutput.size() &&
           (std::isdigit(static_cast<unsigned char>(versionOutput[pos])) || versionOutput[pos] == '.')) {
        verStr += versionOutput[pos++];
    }

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
        case 25: return JavaVersion::Java25;
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
        case JavaVersion::Java25: return 25;
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
        case JavaVersion::Java25: needVer = 25; break;
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
    const fs::path archivePath = runtimeDir / (javaDir.filename().string() + ".zip");

    const std::string url = getDownloadUrl(version);

    log("Downloading Java from: " + url);
    log("Saving to: " + archivePath.string());

    std::error_code ec;
    fs::create_directories(javaDir, ec);
    if (ec) {
        throw std::runtime_error("Failed to create " + javaDir.string() + ": " + ec.message());
    }

    if (fs::exists(archivePath, ec)) {
        fs::remove(archivePath, ec);
    }

    const int MAX_RETRIES = 3;

    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        log("Download attempt " + std::to_string(attempt) + " of " + std::to_string(MAX_RETRIES));

        bool downloaded = http_download_file(url, archivePath);

        if (downloaded && fileExists(archivePath)) {
            uintmax_t size = fs::file_size(archivePath, ec);
            if (!ec && size > 0) {
                log("Download complete: " + std::to_string(size / (1024 * 1024)) + " MB");
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
    fs::path tempJavaDir = javaDir;
    tempJavaDir += ".tmp";
    std::error_code ec;

    try {
        fs::remove_all(tempJavaDir, ec);
        if (ec) {
            throw std::runtime_error("Failed to clear temporary Java directory: " + ec.message());
        }
        fs::create_directories(tempJavaDir, ec);
        if (ec) {
            throw std::runtime_error("Failed to create temporary Java directory: " + ec.message());
        }

        log("Extracting Java to: " + tempJavaDir.string());

        std::string extractErr;
        if (!extractZipWithMiniz(archivePath, tempJavaDir, extractErr)) {
            throw std::runtime_error("Failed to extract Java archive: " + extractErr);
        }

        std::vector<fs::path> subdirs;
        for (const auto& entry : fs::directory_iterator(tempJavaDir, ec)) {
            if (ec) {
                throw std::runtime_error("Failed to list extracted directory: " + ec.message());
            }
            if (entry.is_directory(ec)) {
                subdirs.push_back(entry.path());
            }
        }

        if (subdirs.size() == 1) {
            const fs::path& src = subdirs[0];
            for (const auto& sub : fs::directory_iterator(src, ec)) {
                if (ec) {
                    throw std::runtime_error("Failed to iterate extracted subfolder: " + ec.message());
                }
                fs::path dest = tempJavaDir / sub.path().filename();
                fs::rename(sub.path(), dest, ec);
                if (ec) {
                    throw std::runtime_error("Failed to move " + sub.path().filename().string() +
                                             ": " + ec.message());
                }
            }
            fs::remove(src, ec);
        }

#ifdef _WIN32
        const fs::path javaBinary = tempJavaDir / "bin" / "java.exe";
#else
        const fs::path javaBinary = tempJavaDir / "bin" / "java";
#endif
        if (!fileExists(javaBinary)) {
            throw std::runtime_error("Java binary not found after extraction: " + javaBinary.string());
        }

#ifndef _WIN32
        makeExecutable(javaBinary);
#endif

        verifyJavaInstallation(javaBinary, version);

        fs::path backupJavaDir = javaDir;
        backupJavaDir += ".old";
        const bool hadJavaDir = fs::exists(javaDir, ec);
        if (hadJavaDir) {
            fs::remove_all(backupJavaDir, ec);
            if (ec) {
                throw std::runtime_error("Failed to clear Java backup directory: " + ec.message());
            }
            fs::rename(javaDir, backupJavaDir, ec);
            if (ec) {
                throw std::runtime_error("Failed to preserve existing Java: " + ec.message());
            }
        }

        fs::rename(tempJavaDir, javaDir, ec);
        if (ec) {
            const std::string replace_error = ec.message();
            if (hadJavaDir) {
                std::error_code restore_ec;
                fs::rename(backupJavaDir, javaDir, restore_ec);
            }
            throw std::runtime_error("Failed to replace Java installation: " + replace_error);
        }

        fs::remove_all(backupJavaDir, ec);
        log("Extraction complete.");
    } catch (...) {
        std::error_code cleanup_ec;
        fs::remove_all(tempJavaDir, cleanup_ec);
        throw;
    }
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
    runtimeDir = launcher_paths::runtime_dir();

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
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
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
