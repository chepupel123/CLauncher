// Скопируй ВЕСЬ этот код в src/JavaManager.h:

#pragma once
#include <filesystem>
#include <string>
#include <cstddef>

class JavaManager
{
public:
    JavaManager();
    
    std::filesystem::path ensureJava(const std::string& mcVersion);

private:
    enum class JavaVersion
    {
        Java8,
        Java17,
        Java21
    };
    
    std::filesystem::path runtimeDir;
    std::filesystem::path logPath;
    
    JavaVersion getRequiredJava(const std::string& mcVersion) const;
    
    std::filesystem::path getJavaDirectory(JavaVersion version) const;
    std::filesystem::path getJavaBinary(JavaVersion version) const;
    
    bool findSystemJava(std::filesystem::path& foundPath, JavaVersion required) const;
    
    void installSystemJava(JavaVersion required);
    
    void downloadJava(JavaVersion version);
    
    void extractJava(JavaVersion version, const std::filesystem::path& archivePath);
    
    void verifyJavaInstallation(const std::filesystem::path& javaBinary, 
                               JavaVersion expectedVersion);
    
    void makeExecutable(const std::filesystem::path& javaBinary);
    
    void checkDependencies();
    
    void log(const std::string& message, bool isError = false);
    
    static int parseVersionPart(const std::string& version, std::size_t& pos);
    static std::string shellQuote(const std::string& value);
    static std::string getDownloadUrl(JavaVersion version);
    static int extractMajorJavaVersion(const std::string& versionOutput);
    static bool fileExists(const std::filesystem::path& path);
};
