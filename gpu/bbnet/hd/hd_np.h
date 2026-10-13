// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/hle/np.h. Shared between the NP files of this backend
// (hd_np.cpp, hd_np_matching2.cpp, hd_np_signaling.cpp) and their export table.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"

namespace Hd {

// One of the game's imports this backend answers, by the symbol's name.
struct Export {
    const char* name;
    void* function;
};
using Exports = std::vector<Export>;
#define HD_EXPORT(table, symbol, fn) (table).push_back({symbol, reinterpret_cast<void*>(fn)})

void RegisterNp(Exports& table);          // NpManager, NpAuth, NpWebApi, NpLookup, NpScore
void RegisterNpMatching2(Exports& table);
void RegisterNpSignaling(Exports& table);

// A callback of the game's: up to six integer or pointer arguments, the PS4's calling
// convention. Called on a guest thread of the runtime (the session dispatcher).
using GuestFn = std::int64_t (PS4_SYSV_ABI*)(std::int64_t, std::int64_t, std::int64_t,
                                             std::int64_t, std::int64_t, std::int64_t);
inline std::int64_t CallGuest(void* fn, std::int64_t a0 = 0, std::int64_t a1 = 0,
                              std::int64_t a2 = 0, std::int64_t a3 = 0, std::int64_t a4 = 0,
                              std::int64_t a5 = 0) {
    return reinterpret_cast<GuestFn>(fn)(a0, a1, a2, a3, a4, a5);
}
inline std::int64_t GuestArg(const void* p) {
    return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(p));
}

// 36-byte SceNpId from an online id: the handle (16 chars + NUL), the rest zero.
void NpFillNpId(void* out, const char* online);
// The handle inside a SceNpId, as text.
std::string NpIdText(const void* npid);

// True once the game has started its Matching2 context.
bool NpContextStarted();

// NpSignaling learns a peer's address: fires ESTABLISHED/ACTIVE for a connection that was
// activated before the peer was known.
void NpSignalingPeerKnown(std::uint16_t member_id);
// NpMatching2 tells NpSignaling the room is gone.
void NpSignalingReset();
// The peer left or timed out: DEAD (0) on its connection(s), so the game's SocketState
// closes the link instead of waiting on it.
void NpSignalingPeerDead(const std::string& online_id);

} // namespace Hd
