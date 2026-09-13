#include "launcher.h"
#include <iostream>
#include <curl/curl.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdlib>
#include <string>
static void setup_ca_bundle() {
    wchar_t exe_path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return;
    std::wstring p(exe_path, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    std::wstring cert = p.substr(0, slash + 1) + L"cacert.pem";
    DWORD attr = GetFileAttributesW(cert.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        _wputenv_s(L"SSL_CERT_FILE", cert.c_str());
}
#endif

int main() {
#ifdef _WIN32
    setup_ca_bundle();
#endif
    curl_global_init(CURL_GLOBAL_DEFAULT);
    try {
        Launcher launcher;
        launcher.init();

        while (!launcher.should_close()) {
            launcher.render();
        }

        std::cout << "Launcher closed\n";
        curl_global_cleanup();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }
}
