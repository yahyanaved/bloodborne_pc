// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/hle/np_signaling.cpp.
//
// sceNpSignaling over the peer table.
//
// The game's SocketState FSM (0x10d8bb0) is the only caller: state 4 activates a connection
// to a peer's NpId and expects ESTABLISHED, state 5 reads the status and expects 2 with the
// peer's address and port. The events are the ones the game's handler acts on (the co-op
// contract): 0x01 established (SocketState+0xac), 0x0c active (+0xad), 0x00 dead.
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "hd/hd.h"
#include "hd/hd_np.h"
#include "hd/hd_session.h"

namespace Hd {

namespace {

constexpr int kErrInvalidArg = static_cast<int>(0x80550003);
constexpr std::uint16_t kEvDead = 0x00;
constexpr std::uint16_t kEvEstablished = 0x01;
constexpr std::uint16_t kEvActive = 0x0c;

struct Ctx {
    void* cb = nullptr;
    void* arg = nullptr;
};
struct Conn {
    unsigned ctx = 0;
    std::string online_id;
    std::uint16_t member_id = 0; // 0 until the peer is known
    bool announced = false;      // ESTABLISHED/ACTIVE fired
    bool dead = false;
};

std::mutex g_mu;
std::map<unsigned, Ctx> g_ctx;
std::map<unsigned, Conn> g_conn; // by connection id
unsigned g_next_ctx = 1;
unsigned g_next_conn = 1;

// (ctxId, connId, event, errorCode, arg)
void Fire(unsigned ctx, unsigned conn, std::uint16_t event, int error) {
    Ctx c;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto it = g_ctx.find(ctx);
        if (it == g_ctx.end() || !it->second.cb) return;
        c = it->second;
    }
    if (Cfg().trace) Log("np: NpSignaling event 0x%x conn %u", event, conn);
    CallGuest(c.cb, ctx, conn, event, error, GuestArg(c.arg));
}

// Once a connection has a known peer: ESTABLISHED at +200 ms, ACTIVE at +400.
void AnnounceLocked(unsigned conn_id, Conn& c) {
    if (c.announced || !c.member_id) return;
    c.announced = true;
    PeersSetConn(c.member_id, conn_id, 2);
    const unsigned ctx = c.ctx;
    DispatchAfter(200, Prio::Signaling, [ctx, conn_id] { Fire(ctx, conn_id, kEvEstablished, 0); });
    DispatchAfter(400, Prio::Signaling, [ctx, conn_id] { Fire(ctx, conn_id, kEvActive, 0); });
}

int PS4_SYSV_ABI sig_init(std::uint64_t, int, int, std::uint64_t) {
    Log("sceNpSignalingInitialize");
    return 0;
}
int PS4_SYSV_ABI sig_term() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ctx.clear();
    g_conn.clear();
    return 0;
}
// (const SceNpId*, callback, arg, ctxId*)
int PS4_SYSV_ABI sig_create_ctx(const void*, void* cb, void* arg, unsigned* ctx) {
    if (!ctx) return kErrInvalidArg;
    std::lock_guard<std::mutex> lk(g_mu);
    const unsigned id = g_next_ctx++;
    g_ctx[id] = Ctx{cb, arg};
    *ctx = id;
    Log("sceNpSignalingCreateContext -> %u", id);
    return 0;
}
int PS4_SYSV_ABI sig_delete_ctx(unsigned ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ctx.erase(ctx);
    for (auto it = g_conn.begin(); it != g_conn.end();) {
        it = it->second.ctx == ctx ? g_conn.erase(it) : std::next(it);
    }
    return 0;
}
// (ctxId, const SceNpId* peer, connId*): idempotent per peer, as the game re-activates.
int PS4_SYSV_ABI sig_activate(unsigned ctx, const void* npid, unsigned* conn_out) {
    if (!conn_out) return kErrInvalidArg;
    const std::string peer = NpIdText(npid);
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, c] : g_conn) {
        if (c.ctx == ctx && c.online_id == peer && !c.dead) {
            *conn_out = id;
            if (!c.member_id) {
                Peer p;
                if (PeersFindOnline(peer, &p)) c.member_id = p.member_id;
            }
            AnnounceLocked(id, c);
            return 0;
        }
    }
    const unsigned id = g_next_conn++;
    Conn c;
    c.ctx = ctx;
    c.online_id = peer;
    Peer p;
    if (PeersFindOnline(peer, &p)) c.member_id = p.member_id;
    std::string how = c.member_id ? "" : " (peer not known yet)";
    if (!c.member_id) {
        // The host activates its connection to the guest it summons before the guest is a
        // room member; on PSN the signaling server resolved the address, here ours does,
        // from the peer's context_start.
        std::uint32_t addr = 0;
        std::uint16_t port = 0;
        std::string error;
        if (ServerSignalingResolve(peer, &addr, &port, error)) {
            const Peer prov = PeersProvisional(peer, addr, port);
            c.member_id = prov.member_id;
            char text[64];
            std::snprintf(text, sizeof(text), " (resolved %u.%u.%u.%u:%u)", addr & 0xff,
                          (addr >> 8) & 0xff, (addr >> 16) & 0xff, addr >> 24, port);
            how = text;
        } else {
            how = " (not known and not resolved: " + error + ")";
        }
    }
    g_conn[id] = c;
    *conn_out = id;
    Log("sceNpSignalingActivateConnection %s -> conn %u%s", peer.c_str(), id, how.c_str());
    AnnounceLocked(id, g_conn[id]);
    return 0;
}
int PS4_SYSV_ABI sig_deactivate(unsigned ctx, unsigned conn) {
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_conn.find(conn);
    if (it != g_conn.end() && it->second.ctx == ctx) {
        it->second.dead = true;
        if (it->second.member_id) PeersSetConn(it->second.member_id, conn, 0);
    }
    return 0;
}
// (ctxId, connId, status*, SceNetInAddr*, SceInPort_t*)
int PS4_SYSV_ABI sig_status(unsigned ctx, unsigned conn, int* st, std::uint32_t* addr,
                            std::uint16_t* port) {
    int status = 0;
    std::uint32_t a = 0;
    std::uint16_t p = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto it = g_conn.find(conn);
        if (it != g_conn.end() && it->second.ctx == ctx && !it->second.dead) {
            Peer peer;
            if (it->second.member_id && PeersGet(it->second.member_id, &peer) && peer.addr) {
                status = it->second.announced ? 2 : 1;
                a = peer.addr;
                p = static_cast<std::uint16_t>((peer.port << 8) | (peer.port >> 8));
            } else {
                status = 1;
            }
        }
    }
    if (st) *st = status;
    if (addr) *addr = a;
    if (port) *port = p;
    if (Cfg().trace) Log("sceNpSignalingGetConnectionStatus conn %u -> %d", conn, status);
    return 0;
}

} // namespace

void NpSignalingPeerKnown(std::uint16_t member_id) {
    Peer p;
    if (!PeersGet(member_id, &p)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, c] : g_conn) {
        if (!c.dead && c.online_id == p.online_id) {
            c.member_id = member_id;
            AnnounceLocked(id, c);
        }
    }
}

void NpSignalingPeerDead(const std::string& online_id) {
    std::vector<std::pair<unsigned, unsigned>> dead; // ctx, conn
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& [id, c] : g_conn) {
            if (!c.dead && c.online_id == online_id) {
                c.dead = true;
                dead.emplace_back(c.ctx, id);
            }
        }
    }
    for (const auto& [ctx, conn] : dead) {
        const unsigned ctx_id = ctx, conn_id = conn;
        DispatchAfter(100, Prio::Signaling, [ctx_id, conn_id] { Fire(ctx_id, conn_id, kEvDead, 0); });
    }
}

void NpSignalingReset() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& [id, c] : g_conn) c.dead = true;
}

void RegisterNpSignaling(Exports& t) {
    HD_EXPORT(t, "sceNpSignalingInitialize", sig_init);
    HD_EXPORT(t, "sceNpSignalingTerminate", sig_term);
    HD_EXPORT(t, "sceNpSignalingCreateContext", sig_create_ctx);
    HD_EXPORT(t, "sceNpSignalingDeleteContext", sig_delete_ctx);
    HD_EXPORT(t, "sceNpSignalingActivateConnection", sig_activate);
    HD_EXPORT(t, "sceNpSignalingDeactivateConnection", sig_deactivate);
    HD_EXPORT(t, "sceNpSignalingGetConnectionStatus", sig_status);
}

} // namespace Hd
