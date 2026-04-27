#include "log.h"

#include <atomic>
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

// Off by default. The CLI (--log / --verbose) flips this on.
std::atomic<bool> g_log_enabled{false};

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
    if (!g_log_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(log_mutex());
    std::fprintf(stderr, "[%s] %s  %s\n",
                 timestamp().c_str(), level_tag(level), msg.c_str());
    std::fflush(stderr);
}

void set_log_enabled(bool on) {
    g_log_enabled.store(on, std::memory_order_relaxed);
}

bool is_log_enabled() {
    return g_log_enabled.load(std::memory_order_relaxed);
}

} // namespace ap
