#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ap::airplay {

// Per-session registry of /reverse (PTTH/1.0) sockets.
//
// iOS opens a second TCP connection with `POST /reverse` + `X-Apple-
// Purpose: event`, expecting the server to push AirPlay Streaming
// events back on it (FCUP Request, session end, playback state). The
// socket lives on a different TCP connection from the RTSP control
// socket, so we need a session-ID-keyed global lookup.
//
// The fd is borrowed — the owning Server::handle_client thread keeps
// the socket alive and the session-ID entry until the TCP connection
// closes, at which point handle_client calls unregister.
class ReverseChannelRegistry {
public:
    static ReverseChannelRegistry& instance();

    void register_socket(const std::string& session_id, int fd);
    void unregister_socket(const std::string& session_id);

    // Returns -1 if no reverse channel is open for that session.
    int  socket_for(const std::string& session_id) const;

private:
    ReverseChannelRegistry() = default;

    mutable std::mutex                      mtx_;
    std::unordered_map<std::string, int>    fds_;
};

} // namespace ap::airplay
