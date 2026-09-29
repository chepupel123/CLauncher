#pragma once

#include <string>
#include <sstream>

namespace logger {

enum class Level { Debug, Info, Warn, Error };

void init(const char* argv0);
void write(Level level, const std::string& message);

void  set_min_level(Level level);
Level min_level();

std::string log_file_path();
std::string log_directory();

} // namespace logger

#define LOG_DEBUG(msg)                                                   \
    do {                                                                 \
        std::ostringstream _log_ss_;                                     \
        _log_ss_ << msg;                                                 \
        ::logger::write(::logger::Level::Debug, _log_ss_.str());         \
    } while (0)

#define LOG_INFO(msg)                                                    \
    do {                                                                 \
        std::ostringstream _log_ss_;                                     \
        _log_ss_ << msg;                                                 \
        ::logger::write(::logger::Level::Info, _log_ss_.str());          \
    } while (0)

#define LOG_WARN(msg)                                                    \
    do {                                                                 \
        std::ostringstream _log_ss_;                                     \
        _log_ss_ << msg;                                                 \
        ::logger::write(::logger::Level::Warn, _log_ss_.str());          \
    } while (0)

#define LOG_ERROR(msg)                                                   \
    do {                                                                 \
        std::ostringstream _log_ss_;                                     \
        _log_ss_ << msg;                                                 \
        ::logger::write(::logger::Level::Error, _log_ss_.str());         \
    } while (0)
