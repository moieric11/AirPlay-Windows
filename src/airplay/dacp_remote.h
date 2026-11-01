#pragma once
#include "airplay/media_control.h"
#include <memory>
#include <mutex>
#include <string>

namespace ap::airplay {
// Receiver-to-sender DACP audio controls, independent of mirror/event channels.
class DacpRemote {
public:
    explicit DacpRemote(std::string peer_ip);
    ~DacpRemote();
    void update(const std::string& dacp_id, const std::string& active_remote, bool session_active, const std::string& session_id = {});
    bool available(const std::string& session_id = {});
    bool send(MediaCommand command, uint32_t position_ms = 0, const std::string& session_id = {});
private:
    struct ResolveState;
    std::string peer_ip_;
    std::mutex mutex_;
    std::string dacp_id_, active_remote_, session_id_;
    bool session_active_{false};
    std::shared_ptr<ResolveState> resolve_;
};
}
