#pragma once

#include "airplay/routes.h"

#include <vector>

namespace ap::airplay {

// Build the binary-plist payload returned for `GET /info`.
//
// Structure and field set mirror UxPlay's `plist/info.plist` (iOS 17-safe
// baseline). Values specific to this receiver (deviceid, name, pi, model,
// sourceVersion, features) come from `ctx`; everything else is a known-good
// constant that lets iOS progress past /info into the pair-setup phase.
//
// Returns an empty vector on failure (libplist unavailable or allocation
// error). Caller should fall back to the text placeholder in that case.
std::vector<unsigned char> build_info_plist(const DeviceContext& ctx);

} // namespace ap::airplay
