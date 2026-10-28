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
    // 1. СНАЧАЛА инициализируем логгер. Теперь мы увидим любые ошибки.
    logger::init(argc > 0 ? argv[0] : nullptr);

    // 2. Используем RAII для блокировки. Деструктор сам вызовет release().
    single_instance::InstanceLock instance_lock;
    
    if (!instance_lock.is_acquired()) {
        LOG_WARN("Another instance is already running — exiting");
#ifdef _WIN32
        MessageBoxW(nullptr,
                    L"CLauncher уже запущен.", // Убедись, что файл сохранен в UTF-16 или используй u8"..." 
                    L"CLauncher",
                    MB_OK | MB_ICONINFORMATION);
#endif
        return 0;
    }

    // 3. Инициализация TLS
    curl_tls::initialize();

    try {
        Launcher launcher;
        launcher.init();

        // Главный цикл
        while (!launcher.should_close()) {
            launcher.render();
        }

        LOG_INFO("Launcher closed gracefully");
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal error: " << e.what());
        curl_global_cleanup(); // Очищаем curl при ошибке
        return 1;
    }

    // Очистка при успешном завершении
    curl_global_cleanup(); 
    return 0;
    
    // При выходе из функции сработает деструктор instance_lock и снимет блокировку.
}