#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace ap::airplay {

// AirPlay uses an RTSP-like framing: request line, headers, optional body.
// Both HTTP/1.1 and RTSP/1.0 request lines are seen in the wild — we accept
// both. Requests use CRLF line endings; Content-Length drives body size.
struct Request {
    std::string method;             // "GET", "POST", "SETUP", "RECORD", ...
    std::string uri;                // "/info", "/pair-setup", "rtsp://.../stream", ...
    std::string version;            // "HTTP/1.1" or "RTSP/1.0"
    std::unordered_map<std::string, std::string> headers;  // lowercase keys
    std::vector<unsigned char> body;

    std::string header(const std::string& key) const;  // "" if missing
    std::size_t content_length() const;
};

struct Response {
    int status_code = 200;
    std::string status_text = "OK";
    std::string version = "RTSP/1.0";
    std::unordered_map<std::string, std::string> headers;
    std::vector<unsigned char> body;

    void set_header(std::string key, std::string value);
    std::string serialize() const;
};

// Blocking read of one full request from `fd`. Returns false on disconnect
// or protocol error. On success, `out` is fully populated and the body has
// exactly Content-Length bytes (0 if no header).
class RequestReader {
public:
    bool read(int fd, Request& out);

private:
    std::string buffer_;   // accumulated bytes across recv calls
    bool fill(int fd, std::size_t need);
    bool read_headers(int fd, Request& out, std::size_t& body_offset);
};

} // namespace ap::airplay
