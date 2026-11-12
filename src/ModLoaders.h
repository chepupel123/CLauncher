#pragma once
#include "MinecraftInstaller.h"

namespace MinecraftInstaller {
    std::string installFabric(const std::string& version);
    std::string installForge(const std::string& mc_version);
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
