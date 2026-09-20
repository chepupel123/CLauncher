#pragma once
#include <string>

// Лёгкий потокобезопасный логгер для CLauncher.
//
// Пишет одновременно:
//   * в файл clauncher.log рядом с исполняемым файлом (или в папку данных
//     лаунчера, если рядом с exe нет прав на запись);
//   * в консоль (stdout для INFO/WARN, stderr для ERROR).
//
// Формат в файле: [2026-09-20 21:09:12] [INFO] message
// Уровни: INFO, WARN, ERROR.
//
// Использование:
//     LOG_INFO("Manifest cached: " << path);
//     LOG_WARN("Retrying download");
//     LOG_ERROR("Curl failed: " << curl_easy_strerror(res));
// Это safe даже до инициализации: до вызова Logger::early_init() сообщения
// попадают только в консоль (и буферизуются для файла).

#include <sstream>

namespace logger {

enum class Level { Info, Warn, Error };

// Вызывается в самом начале main(). Определяет путь к лог-файлу.
void init(const char* argv0);

// Пишет одну строку в лог (файл + консоль). Потокобезопасна.
void write(Level level, const std::string& message);

// Путь к текущему лог-файлу (для кнопки «открыть лог» в UI).
std::string log_file_path();

// Название папки, в которой лежит лог-файл.
std::string log_directory();

} // namespace logger

// ---- Макросы-обёртки (RAII, без raw new/delete) ----
#define LOG_INFO(msg)                                                    \
    do {                                                                \
        std::ostringstream _log_ss_;                                    \
        _log_ss_ << msg;                                                \
        ::logger::write(::logger::Level::Info, _log_ss_.str());         \
    } while (0)

#define LOG_WARN(msg)                                                    \
    do {                                                                \
        std::ostringstream _log_ss_;                                    \
        _log_ss_ << msg;                                                \
        ::logger::write(::logger::Level::Warn, _log_ss_.str());         \
    } while (0)

#define LOG_ERROR(msg)                                                   \
    do {                                                                \
        std::ostringstream _log_ss_;                                    \
        _log_ss_ << msg;                                                \
        ::logger::write(::logger::Level::Error, _log_ss_.str());        \
    } while (0)
