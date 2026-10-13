// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/engine/summon_invite.h, sos_signs.h, playlog.h, chara_id.h.
//
// What the online path needs from the game's own memory. Every address is Bloodborne 1.09's
// (the eboot the community patches are for); with another build all of these are no-ops.
#pragma once

#include <cstdint>

#include "hd/hd_http.h"

namespace Hd::Engine {

// The loaded image, still writable: binds the addresses and applies the startup patches
// (the summon sign timeout, the play log's periods).
void SetImage(void* image, std::uint64_t size);

// The NpMatching2 signaling gate (data_586cc09): the game's callbacks queue events only
// when it is 1; on PSN the library set it.
void SetSignalingGate();

// The host's side of a summon: area, position and level for the invite the server relays,
// read from FrpgNetMan. Adds HostArea/HostPos/HostLevel/MemberTag.
void HostExtra(Json& extra);

// A character whose id the server never issued holds 0x8000000000000000 where the id
// belongs, and the game would send that as its CharaId for good. Cleared to 0 ("none"), the
// network login asks the server for one. Cheap; called from the calls the login makes.
void FixCharaId();

} // namespace Hd::Engine
