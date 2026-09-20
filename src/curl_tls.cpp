#include "curl_tls.h"
#include "logger.h"

#include <curl/curl.h>

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace curl_tls {
namespace {

std::once_flag g_once;
Result g_result;      // заполняется один раз в initialize()
bool g_native_ca = false;
std::string g_ca_info_path;

// Путь к каталогу с исполняемым файлом (там же лежит cacert.pem).
fs::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return fs::path(std::wstring(buf, n)).parent_path();
    }
    return fs::path(".");
#else
    return fs::current_path();
#endif
}

// Пытаемся включить системное хранилище Windows (Schannel).
// Возвращает true, если libcurl поддержал эту опцию.
bool try_native_ca() {
#if LIBCURL_VERSION_NUM >= 0x074700  // 7.71.0
    // Пробный хэндл: если build libcurl без Schannel или собрана с OpenSSL,
    // настройка может тихо не примениться — проверяем фактически.
    CURL* probe = curl_easy_init();
    if (!probe) return false;
    CURLcode rc = curl_easy_setopt(probe, CURLOPT_SSL_OPTIONS,
                                   static_cast<long>(CURLSSLOPT_NATIVE_CA));
    curl_easy_cleanup(probe);
    return rc == CURLE_OK;
#else
    return false;
#endif
}

} // namespace

// Используем ли мы системное хранилище Windows? Определяем по бэкенду TLS:
// NATIVE_CA работает только со Schannel. Если бэкенд OpenSSL (типично для
// MinGW-сборок), NATIVE_CA будет «тихо проигнорирован» — поэтому честно
// проверяем ssl_version и в этом случае сразу идём в cacert.pem.
static bool curl_uses_schannel() {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->ssl_version) return false;
    return std::string(info->ssl_version).find("Schannel") != std::string::npos;
}

Result initialize() {
    std::call_once(g_once, [] {
        const CURLcode g = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (g != CURLE_OK) {
            g_result.error = std::string("curl_global_init failed: ") + curl_easy_strerror(g);
            LOG_ERROR("curl_global_init failed: " << curl_easy_strerror(g));
            return;
        }

#ifdef _WIN32
        // Windows: сначала системное хранилище (Schannel), потом cacert.pem.
        if (!curl_uses_schannel()) {
            LOG_WARN("TLS backend is not Schannel (ssl_version='" <<
                     (curl_version_info(CURLVERSION_NOW) &&
                      curl_version_info(CURLVERSION_NOW)->ssl_version ?
                      curl_version_info(CURLVERSION_NOW)->ssl_version : "?") <<
                     "'), so NATIVE_CA is unavailable");
            LOG_WARN("falling back to cacert.pem next to the exe");
        } else if (try_native_ca()) {
            g_native_ca = true;
            g_result.native_ca_active = true;
            LOG_INFO("TLS: using Windows native CA store (Schannel) via CURLSSLOPT_NATIVE_CA");
        }
#endif

        if (!g_native_ca) {
            // Ищем cacert.pem рядом с exe (как и раньше в main.cpp,
            // но теперь через CURLOPT_CAINFO + глобально для всех хэндлов).
            const fs::path cert = exe_dir() / "cacert.pem";
            std::error_code ec;
            if (fs::exists(cert, ec) && !ec) {
                g_ca_info_path = cert.string();
                g_result.ca_info_active = true;
                LOG_INFO("TLS: using CA bundle: " << g_ca_info_path);
            } else {
#ifndef _WIN32
                // На Linux системные CA находятся автоматически — это норма.
                LOG_INFO("TLS: no local cacert.pem; relying on system CA store");
#else
                g_result.error =
                    "No TLS CA source available: neither the Windows native CA "
                    "store nor " + cert.string() + " (cacert.pem) was found.";
                LOG_ERROR(g_result.error);
                LOG_ERROR("HTTPS downloads WILL fail — copy cacert.pem next to the exe "
                          "or use a libcurl build with Schannel support.");
#endif
            }
        }
    });
    return g_result;
}

bool apply(void* curl_easy_handle) {
    CURL* curl = static_cast<CURL*>(curl_easy_handle);
    if (!curl) return false;

    std::call_once(g_once, [] { initialize(); });

    // Никогда не отключаем проверку сертификатов (безопасность превыше всего).
    if (g_native_ca) {
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS,
                         static_cast<long>(CURLSSLOPT_NATIVE_CA));
        return true;
    }
    if (!g_ca_info_path.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, g_ca_info_path.c_str());
        return true;
    }
    // На Linux системные CA находятся сами — true.
    // На Windows это уже зафиксировано в g_result.error выше.
    return g_result.error.empty();
}

} // namespace curl_tls
