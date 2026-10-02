#pragma once
#include <string>

namespace JavaLauncher {
    bool launch(const std::string& nickname_raw,
                const std::string& version,
                int memory_mb,
                bool wait_for_exit = false);

    void open_url(const std::string& url);
}
