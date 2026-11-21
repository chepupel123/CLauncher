#include "ProcessRunner.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string_view>

static std::wstring widen_utf8_bytes(const char* data, size_t size) {
    if (!data || size == 0) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), w.data(), n);
    return w;
}

static std::wstring widen(std::string_view s) {
    return widen_utf8_bytes(s.data(), s.size());
}

static std::wstring widen(std::u8string_view s) {
    return widen_utf8_bytes(reinterpret_cast<const char*>(s.data()), s.size());
}

// Корректное квотирование аргументов по правилам MSVCRT
static std::wstring quote_arg(const std::wstring& a) {
    if (a.empty()) return L"\"\"";
    bool need = a.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!need) return a;
    std::wstring out = L"\"";
    int bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') ++bs;
        else if (c == L'"') { out.append(bs * 2 + 1, L'\\'); out.push_back(L'"'); bs = 0; }
        else { if (bs) { out.append(bs, L'\\'); bs = 0; } out.push_back(c); }
    }
    if (bs) out.append(bs * 2, L'\\');
    out.push_back(L'"');
    return out;
}

namespace process {

ProcessResult run(const fs::path& exe, const std::vector<std::string>& args,
                  const fs::path& log_file, int timeout_seconds) {
    ProcessResult r;

    std::wstring cmd = quote_arg(widen(exe.u8string()));
    for (auto& a : args) { cmd.push_back(L' '); cmd.append(quote_arg(widen(a))); }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(widen(log_file.u8string()).c_str(),
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        r.error = "CreateFileW failed: " + std::to_string(GetLastError());
        return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hLog;
    si.hStdError  = hLog;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hLog);

    if (!ok) {
        r.error = "CreateProcessW failed: " + std::to_string(GetLastError());
        return r;
    }

    DWORD waitMs = timeout_seconds > 0 ? (DWORD)timeout_seconds * 1000 : INFINITE;
    DWORD wr = WaitForSingleObject(pi.hProcess, waitMs);
    if (wr == WAIT_TIMEOUT) {
        r.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = (int)code;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return r;
}

} // namespace process

#else // POSIX

#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>

namespace process {

ProcessResult run(const fs::path& exe, const std::vector<std::string>& args,
                  const fs::path& log_file, int timeout_seconds) {
    ProcessResult r;

    int fd = ::open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        r.error = "open failed: " + std::string(strerror(errno));
        return r;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fd);
        r.error = "fork failed: " + std::string(strerror(errno));
        return r;
    }
    if (pid == 0) {
        // Дочерний процесс
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);

        std::string exe_s = exe.string();
        std::vector<std::string> a = args;
        std::vector<char*> argv;
        argv.push_back(exe_s.data());
        for (auto& s : a) argv.push_back(s.data());
        argv.push_back(nullptr);

        ::execv(exe_s.c_str(), argv.data());
        ::_exit(127); // Если execv вернулся — ошибка
    }

    // Родительский процесс
    ::close(fd);
    int status = 0;

    if (timeout_seconds > 0) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::seconds(timeout_seconds);
        bool done = false;

        while (std::chrono::steady_clock::now() < deadline) {
            pid_t w = ::waitpid(pid, &status, WNOHANG);
            if (w == pid) { done = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!done) {
            r.timed_out = true;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
    } else {
        ::waitpid(pid, &status, 0);
    }

    if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.exit_code = 128 + WTERMSIG(status);
    }

    return r;
}

} // namespace process

#endif // _WIN32
