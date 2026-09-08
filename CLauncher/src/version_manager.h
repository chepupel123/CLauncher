#pragma once
#include <string>
#include <vector>

class VersionManager {
public:
    struct Version {
        std::string id;
        std::string type;
        std::string url;
        std::string sha1;
        std::string time;
        std::string releaseTime;
    };

    static std::string get_manifest_cache_path();
    static bool has_manifest_cache();
    static long get_cache_timestamp();
    static bool fetch_and_cache_manifest();
    static std::vector<Version> parse_manifest();
    static std::vector<std::string> get_release_versions();
};
