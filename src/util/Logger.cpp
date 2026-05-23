#include "util/Logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace weaknet {
namespace {

std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};

#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T");
    return oss.str();
}

void log_line(const char* level, const std::string& message, std::ostream& stream)
{
    stream << "[" << timestamp() << "] [" << level << "] " << message << std::endl;
}

} // namespace

void Logger::info(const std::string& message)
{
    log_line("INFO", message, std::cerr);
}

void Logger::warn(const std::string& message)
{
    log_line("WARN", message, std::cerr);
}

void Logger::error(const std::string& message)
{
    log_line("ERROR", message, std::cerr);
}

} // namespace weaknet
