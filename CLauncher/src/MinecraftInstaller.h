#pragma once
#include <string>
#include <unordered_map>

namespace MinecraftInstaller
{
    using ProgressFn = void (*)(int percent, const char* stage, void* user);
    void set_progress_callback(ProgressFn fn, void* user);

    bool install(const std::string& version);
    std::string installFabric(const std::string& version);

    bool installPerformanceMods(const std::string& fabric_version_name,
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
