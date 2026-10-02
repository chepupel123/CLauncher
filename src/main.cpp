#include "launcher.h"
#include "curl_tls.h"
#include "logger.h"
#include "single_instance.h"

#include <iostream>
#include <curl/curl.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

int main(int argc, char** argv) {
    // Лог-файл: clauncher.log рядом с исполняемым файлом (или в папке данных
    // лаунчера, если рядом с exe нет прав на запись).
    if (!single_instance::acquire()) {
        LOG_WARN("Another instance is already running — exiting");
#ifdef _WIN32
        MessageBoxW(nullptr,
                    L"CLauncher уже запущен.",
                    L"CLauncher",
                    MB_OK | MB_ICONINFORMATION);
#endif
        return 0;
    }

    logger::init(argc > 0 ? argv[0] : nullptr);

    // TLS: на Windows сначала настраиваем системное хранилище CA
    // (CURLSSLOPT_NATIVE_CA, libcurl >= 7.71), затем cacert.pem рядом с exe.
    // На Linux поведение не меняется — системные CA находятся автоматически.
    // Проверка сертификатов никогда не отключается.
    curl_tls::initialize();

    try {
        Launcher launcher;
        launcher.init();

        while (!launcher.should_close()) {
            launcher.render();
        }

        LOG_INFO("Launcher closed");
        curl_global_cleanup();
        single_instance::release();
        return 0;
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: " << e.what());
        curl_global_cleanup();
        single_instance::release();
        return 1;
    }
}
