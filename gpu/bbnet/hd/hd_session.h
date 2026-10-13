// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/net/session.h.
//
// The session service behind the Sony NP API.
//
// Three parts, none of which knows the SDK's structs: a Dispatcher (the one thread that
// calls guest callbacks, in due-time and priority order), a Poller (asks the private
// server's /np/events queue for matching2 events and hands them to whoever registered), and
// the PeerTable (member id -> addresses and signaling state, shared by NpMatching2 and
// NpSignaling). The transport is the private server's /mp/matching2/* endpoints over
// hd_http.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "hd/hd_http.h"

namespace Hd {

// Priority when several callbacks are due at once: the contract's order.
enum class Prio { Context = 0, Request = 1, Signaling = 2, RoomEvent = 3 };

// Runs fn on the dispatcher thread after delay_ms. The thread is a guest thread of the
// runtime, so fn may call the game's callbacks. Serial: no two run at once, and the queue
// is drained in (due, priority, order) order.
void DispatchAfter(int delay_ms, Prio prio, std::function<void()> fn);
// True on the dispatcher thread.
bool OnDispatcher();
// Stops the dispatcher and the poller (exit).
void SessionShutdown();

// The online id this player plays as (the account's name, else BB_HD_ONLINE_ID, else "Player").
std::string OnlineId();
// Our P2P endpoint as the server should record it (BB_HD_P2P_PORT, or the port this
// instance actually bound when another program had that one).
std::string LocalAddrText();
int P2pPortNumber();
void SetP2pPortBound(int port);

struct Peer {
    std::uint16_t member_id = 0;
    std::string online_id;
    std::uint32_t addr = 0;       // network byte order, the address other peers reach us at
    std::uint16_t port = 0;       // host byte order
    std::uint32_t local_addr = 0; // network byte order
    std::uint16_t local_port = 0;
    unsigned conn_id = 0;         // NpSignaling connection id once activated (0 = none)
    int sig_status = 0;           // 0 inactive, 1 pending, 2 active
};

// The peer table. All calls lock internally.
void PeersClear();
void PeersUpsert(const Peer& p); // by member_id; keeps conn_id/sig_status when the caller left them 0
bool PeersGet(std::uint16_t member_id, Peer* out);
bool PeersFindOnline(const std::string& online_id, Peer* out);
bool PeersFindConn(unsigned conn_id, Peer* out);
void PeersSetConn(std::uint16_t member_id, unsigned conn_id, int sig_status);
std::vector<Peer> PeersAll();
void PeersErase(std::uint16_t member_id);
std::int64_t NowMs();
// Parses the server's member records ({MemberId, OnlineId, Addr, Port, LocalAddr,
// LocalPort, ...}) into the table. Returns how many.
int PeersFromMembers(const Json& members);

// The server. Each returns false with `error` on failure; `reply` is the parsed JSON.
bool ServerContextStart(Json& reply, std::string& error);
bool ServerCreateRoom(int max_members, const Json& extra, Json& reply, std::string& error);
bool ServerJoinRoom(std::uint64_t room_id, Json& reply, std::string& error);
bool ServerLeaveRoom(const std::string& session_id, int member_id, Json& reply,
                     std::string& error);
// 1 when the server keeps us in the room, 0 when it says we are not in it (it dropped the
// room or us: "InRoom": 0), -1 when it could not be asked.
int ServerHeartbeat(const std::string& session_id, int member_id);
// The host's KickoutRoomMember: `opt` is the request's optData (up to 16 bytes), which the
// kicked player's game reads.
bool ServerKickMember(const std::string& session_id, int member_id, int kicker_id,
                      const std::uint8_t* opt, std::size_t opt_len, std::string& error);
bool ServerSessionBlob(Json& reply, std::string& error);
// The peer's signaling address as the server learned it at its context_start
// (/np/signaling/resolve). addr in network byte order, port in host order.
bool ServerSignalingResolve(const std::string& online_id, std::uint32_t* addr,
                            std::uint16_t* port, std::string& error);
// A peer known by address before it is a room member (the host activates a connection to
// the guest it summons before the guest joins). Takes a member id from the provisional
// range 0xff00.. so the table can hold it; the real member record replaces nothing - both
// entries stay, the connection keeps the provisional one and the address is the same.
Peer PeersProvisional(const std::string& online_id, std::uint32_t addr, std::uint16_t port);

// The event poller: every interval it asks /np/events/poll for the next matching2 event
// for our online id, hands it to the handler, and acks it. The handler runs on the poller
// thread and must not call guest code; it schedules through DispatchAfter.
void PollerStart(std::function<void(const Json& event)> handler);
void PollerStop();

// The room we are in, for the keepalive's address updates (hd_np_matching2 sets it when a
// room is made or joined and clears it when it is over).
void RoomSet(const std::string& session_id, int member_id);
void RoomClear();
// The last STUN exchange's round trip to the server, microseconds (0 = none yet).
int StunRttUs();

} // namespace Hd
