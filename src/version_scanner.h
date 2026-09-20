#pragma once
#include <string>
#include <vector>

class VersionScanner {
public:
    static std::string get_versions_path();
    static std::vector<std::string> scan_versions();
};
