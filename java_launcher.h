#pragma once
#include <string>

namespace JavaLauncher {
    bool launch(const std::string& nickname, 
                const std::string& version, 
                int memory_mb);
    
    void open_url(const std::string& url);
}
