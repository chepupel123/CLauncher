#pragma once

#include <string>

// Lightweight thread-safe logger for CLauncher.
//
// Writes simultaneously:
//   * to file clauncher.log next to the executable (or in the launcher data
//     folder, if next to exe there are no write permissions);
//   * to console (stdout for INFO/WARN, stderr for ERROR).
//
// Format in file: [2026-09-20 21:09:12] [INFO] message
// Levels: DEBUG, INFO, WARN, ERROR.
//
// Usage:
// LOG_INFO("Manifest cached: " << path);
// LOG_WARN("Retrying download");
// LOG_ERROR("Curl failed: " << curl_easy_strerror(res));
// This is safe even before initialization: before Logger::init() messages
// go only to console (and are buffered for the file).

#include <sstream>

namespace logger {

enum class Level { Debug, Info, Warn, Error };

// Called at the very beginning of main(). Determines the path to the log file.
void init(const char* argv0);

// Writes a single line to the log (file + console). Thread-safe.
void write(Level level, const std::string& message);

// Path to the current log file (for a "open log" button in UI).
std::string log_file_path();

// Name of the folder in which the log file resides.
std::string log_directory();

// Global log level threshold (default Info). Messages below this level are dropped.
extern Level g_log_level;

} // namespace logger

// ---- Macros wrappers (RAII, no raw new/delete) ----

#define LOG_DEBUG(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Debug, _log_ss_.str()); \
} while (0)

#define LOG_INFO(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Info, _log_ss_.str()); \
} while (0)

#define LOG_WARN(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Warn, _log_ss_.str()); \
} while (0)

#define LOG_ERROR(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Error, _log_ss_.str()); \
} while (0)#pragma once

#include <string>

// Lightweight thread-safe logger for CLauncher.
//
// Writes simultaneously:
//   * to file clauncher.log next to the executable (or in the launcher data
//     folder, if next to exe there are no write permissions);
//   * to console (stdout for INFO/WARN, stderr for ERROR).
//
// Format in file: [2026-09-20 21:09:12] [INFO] message
// Levels: DEBUG, INFO, WARN, ERROR.
//
// Usage:
// LOG_INFO("Manifest cached: " << path);
// LOG_WARN("Retrying download");
// LOG_ERROR("Curl failed: " << curl_easy_strerror(res));
// This is safe even before initialization: before Logger::init() messages
// go only to console (and are buffered for the file).

#include <sstream>

namespace logger {

enum class Level { Debug, Info, Warn, Error };

// Called at the very beginning of main(). Determines the path to the log file.
void init(const char* argv0);

// Writes a single line to the log (file + console). Thread-safe.
void write(Level level, const std::string& message);

// Path to the current log file (for a "open log" button in UI).
std::string log_file_path();

// Name of the folder in which the log file resides.
std::string log_directory();

// Global log level threshold (default Info). Messages below this level are dropped.
extern Level g_log_level;

} // namespace logger

// ---- Macros wrappers (RAII, no raw new/delete) ----

#define LOG_DEBUG(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Debug, _log_ss_.str()); \
} while (0)

#define LOG_INFO(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Info, _log_ss_.str()); \
} while (0)

#define LOG_WARN(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Warn, _log_ss_.str()); \
} while (0)

#define LOG_ERROR(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Error, _log_ss_.str()); \
} while (0)#pragma once

#include <string>

// Lightweight thread-safe logger for CLauncher.
//
// Writes simultaneously:
//   * to file clauncher.log next to the executable (or in the launcher data
//     folder, if next to exe there are no write permissions);
//   * to console (stdout for INFO/WARN, stderr for ERROR).
//
// Format in file: [2026-09-20 21:09:12] [INFO] message
// Levels: DEBUG, INFO, WARN, ERROR.
//
// Usage:
// LOG_INFO("Manifest cached: " << path);
// LOG_WARN("Retrying download");
// LOG_ERROR("Curl failed: " << curl_easy_strerror(res));
// This is safe even before initialization: before Logger::init() messages
// go only to console (and are buffered for the file).

#include <sstream>

namespace logger {

enum class Level { Debug, Info, Warn, Error };

// Called at the very beginning of main(). Determines the path to the log file.
void init(const char* argv0);

// Writes a single line to the log (file + console). Thread-safe.
void write(Level level, const std::string& message);

// Path to the current log file (for a "open log" button in UI).
std::string log_file_path();

// Name of the folder in which the log file resides.
std::string log_directory();

// Global log level threshold (default Info). Messages below this level are dropped.
extern Level g_log_level;

} // namespace logger

// ---- Macros wrappers (RAII, no raw new/delete) ----

#define LOG_DEBUG(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Debug, _log_ss_.str()); \
} while (0)

#define LOG_INFO(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Info, _log_ss_.str()); \
} while (0)

#define LOG_WARN(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Warn, _log_ss_.str()); \
} while (0)

#define LOG_ERROR(msg) \
do { \
    std::ostringstream _log_ss_; \
    _log_ss_ << msg; \
    ::logger::write(::logger::Level::Error, _log_ss_.str()); \
} while (0)
