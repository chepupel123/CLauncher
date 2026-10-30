#pragma once
#include <string>
#include "instance.h"

namespace JavaLauncher {
    bool launch(const std::string& nickname_raw,
                const std::string& version_id,
                int memory_mb,
                bool wait_for_exit = false);

    bool launch(const Instance& inst, bool wait_for_exit = false);
    void open_url(const std::string& url);
}
