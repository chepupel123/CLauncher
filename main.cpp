#include "launcher.h"
#include <iostream>
#include <curl/curl.h>   // <-- добавить

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);  // <-- добавить
    try {
        Launcher launcher;
        launcher.init();

        while (!launcher.should_close()) {
            launcher.render();
        }

        std::cout << "Launcher closed\n";
        curl_global_cleanup();              // <-- добавить
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        curl_global_cleanup();              // <-- добавить
        return 1;
    }
}
