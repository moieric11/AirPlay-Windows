#include "log.h"

#include <chrono>
#include <ctime>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace ap {
namespace {

std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

const char* level_tag(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

std::string timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t   = system_clock::to_time_t(now);
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%H:%M:%S") << '.'
       << std::setfill('0') << std::setw(3) << ms.count();
    return os.str();
}

} // namespace

void log_write(LogLevel level, const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex());
    std::fprintf(stderr, "[%s] %s  %s\n",
                 timestamp().c_str(), level_tag(level), msg.c_str());
    std::fflush(stderr);
}

} // namespace ap
