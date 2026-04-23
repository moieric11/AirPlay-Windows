#include "airplay/reverse_channel.h"
#include "log.h"

namespace ap::airplay {

ReverseChannelRegistry& ReverseChannelRegistry::instance() {
    static ReverseChannelRegistry inst;
    return inst;
}

void ReverseChannelRegistry::register_socket(const std::string& session_id, int fd) {
    if (session_id.empty() || fd < 0) return;
    std::lock_guard<std::mutex> lock(mtx_);
    fds_[session_id] = fd;
    LOG_INFO << "reverse-channel: registered session=" << session_id << " fd=" << fd;
}

void ReverseChannelRegistry::unregister_socket(const std::string& session_id) {
    if (session_id.empty()) return;
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = fds_.find(session_id);
    if (it != fds_.end()) {
        LOG_INFO << "reverse-channel: unregistered session=" << session_id
                 << " fd=" << it->second;
        fds_.erase(it);
    }
}

int ReverseChannelRegistry::socket_for(const std::string& session_id) const {
    if (session_id.empty()) return -1;
    std::lock_guard<std::mutex> lock(mtx_);
    const auto it = fds_.find(session_id);
    return it == fds_.end() ? -1 : it->second;
}

} // namespace ap::airplay
