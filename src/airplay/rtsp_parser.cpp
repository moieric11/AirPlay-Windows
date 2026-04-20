#include "airplay/rtsp_parser.h"
#include "net/socket.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace ap::airplay {
namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t");
    auto b = s.find_last_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

} // namespace

std::string Request::header(const std::string& key) const {
    auto it = headers.find(to_lower(key));
    return it == headers.end() ? std::string{} : it->second;
}

std::size_t Request::content_length() const {
    auto v = header("content-length");
    if (v.empty()) return 0;
    try { return static_cast<std::size_t>(std::stoul(v)); }
    catch (...) { return 0; }
}

void Response::set_header(std::string key, std::string value) {
    headers[std::move(key)] = std::move(value);
}

std::string Response::serialize() const {
    std::ostringstream os;
    os << version << ' ' << status_code << ' ' << status_text << "\r\n";
    for (const auto& [k, v] : headers) {
        os << k << ": " << v << "\r\n";
    }
    os << "Content-Length: " << body.size() << "\r\n";
    os << "\r\n";
    std::string head = os.str();
    head.append(reinterpret_cast<const char*>(body.data()), body.size());
    return head;
}

bool RequestReader::fill(int fd, std::size_t need) {
    char tmp[4096];
    while (buffer_.size() < need) {
        int n = ::recv(static_cast<::socket_t>(fd), tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buffer_.append(tmp, n);
    }
    return true;
}

bool RequestReader::read_headers(int fd, Request& out, std::size_t& body_offset) {
    // Grow buffer_ until we see CRLFCRLF.
    char tmp[4096];
    while (true) {
        auto pos = buffer_.find("\r\n\r\n");
        if (pos != std::string::npos) {
            body_offset = pos + 4;
            break;
        }
        int n = ::recv(static_cast<::socket_t>(fd), tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buffer_.append(tmp, n);
        if (buffer_.size() > 64 * 1024) {
            LOG_WARN << "header section too large, dropping connection";
            return false;
        }
    }

    // Parse request line.
    auto line_end = buffer_.find("\r\n");
    std::string line = buffer_.substr(0, line_end);
    std::istringstream ls(line);
    if (!(ls >> out.method >> out.uri >> out.version)) {
        LOG_WARN << "malformed request line: " << line;
        return false;
    }

    // Parse headers.
    std::size_t p = line_end + 2;
    while (p < body_offset - 2) {
        auto eol = buffer_.find("\r\n", p);
        if (eol == std::string::npos || eol > body_offset - 2) break;
        std::string h = buffer_.substr(p, eol - p);
        p = eol + 2;
        if (h.empty()) break;
        auto colon = h.find(':');
        if (colon == std::string::npos) continue;
        std::string key   = to_lower(trim(h.substr(0, colon)));
        std::string value = trim(h.substr(colon + 1));
        out.headers[key] = value;
    }
    return true;
}

bool RequestReader::read(int fd, Request& out) {
    out = {};

    std::size_t body_offset = 0;
    if (!read_headers(fd, out, body_offset)) return false;

    std::size_t clen = out.content_length();
    std::size_t need = body_offset + clen;
    if (buffer_.size() < need) {
        if (!fill(fd, need)) return false;
    }

    out.body.assign(
        reinterpret_cast<const unsigned char*>(buffer_.data() + body_offset),
        reinterpret_cast<const unsigned char*>(buffer_.data() + body_offset + clen));

    // Shift leftover bytes (pipelined requests) to the front.
    buffer_.erase(0, need);
    return true;
}

} // namespace ap::airplay
