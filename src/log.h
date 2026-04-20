#pragma once

#include <sstream>
#include <string>

namespace ap {

enum class LogLevel { Debug, Info, Warn, Error };

void log_write(LogLevel level, const std::string& msg);

class LogStream {
public:
    explicit LogStream(LogLevel level) : level_(level) {}
    ~LogStream() { log_write(level_, buf_.str()); }

    template <typename T>
    LogStream& operator<<(const T& value) {
        buf_ << value;
        return *this;
    }

private:
    LogLevel level_;
    std::ostringstream buf_;
};

} // namespace ap

#define LOG_DEBUG ::ap::LogStream(::ap::LogLevel::Debug)
#define LOG_INFO  ::ap::LogStream(::ap::LogLevel::Info)
#define LOG_WARN  ::ap::LogStream(::ap::LogLevel::Warn)
#define LOG_ERROR ::ap::LogStream(::ap::LogLevel::Error)
