#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct ProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string error;
};

namespace process {
    ProcessResult run(const fs::path& exe,
                      const std::vector<std::string>& args,
                      const fs::path& log_file,
                      int timeout_seconds = 0);
}
