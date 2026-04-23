#pragma once

#include <string>

namespace ap::airplay {

// Send a POST /event FCUP Request on the already-open /reverse socket
// `fd`. Returns false on send failure or libplist missing at build.
// `url` is the mlhls://localhost/... URL iOS handed us in POST /play;
// iOS will fetch the corresponding YouTube content on its end and push
// the bytes back via POST /action with matching request_id.
bool send_fcup_request(int fd,
                       const std::string& url,
                       const std::string& session_id,
                       int request_id);

} // namespace ap::airplay
