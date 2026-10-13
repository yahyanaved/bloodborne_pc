// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/hle/np_matching2.cpp.
//
// sceNpMatching2 over the session service.
//
// The game takes three values from this library - connection status 2, the peer's address,
// the peer's port - and its own session state machines do the rest, driven by the callbacks
// below. The layouts are the SDK's; what the game reads out of each callback was read from
// the eboot's own dispatch (sub_10c3210: the roomId at RoomDataInternal+0x18, the member
// list, the owner's member id; sub_10c3da0: MEMBER_JOINED 0x1101 with the member record at
// data[0]). Timings and order are the co-op contract.
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "hd/hd.h"
#include "hd/hd_account.h"
#include "hd/hd_engine.h"
#include "hd/hd_np.h"
#include "hd/hd_session.h"

namespace Hd {

namespace {

// ---- SDK layouts (the game's build of libSceNpMatching2) --------------------

struct RoomDataInternal {
    std::uint16_t publicSlots, privateSlots, openPublicSlots, openPrivateSlots, maxSlot, serverId;
    std::uint32_t worldId;
    std::uint64_t lobbyId;
    std::uint64_t roomId;
    std::uint64_t passwdSlotMask;
    std::uint64_t joinedSlotMask;
    const void* roomGroup;
    std::uint64_t roomGroups;
    std::uint32_t flags;
    std::uint32_t pad;
    const void* roomBinAttrInternal;
    std::uint64_t roomBinAttrInternalNum;
};
static_assert(offsetof(RoomDataInternal, roomId) == 0x18);
static_assert(sizeof(RoomDataInternal) == 0x58);

struct RoomMemberDataInternal {
    RoomMemberDataInternal* next;
    std::uint64_t joinDate;
    std::uint8_t npId[36];
    std::uint8_t pad[4];
    std::uint16_t memberId;
    std::uint8_t teamId;
    std::uint8_t natType;
    std::uint32_t flagAttr;
    void* roomGroup;
    void* roomMemberBinAttrInternal;
    std::uint64_t roomMemberBinAttrInternalNum;
};
static_assert(offsetof(RoomMemberDataInternal, memberId) == 0x38);
static_assert(sizeof(RoomMemberDataInternal) == 0x58);

struct RoomMemberDataInternalList {
    RoomMemberDataInternal* members;
    std::uint64_t membersNum;
    RoomMemberDataInternal* me;
    RoomMemberDataInternal* owner;
};

struct CreateJoinRoomResponse {
    const RoomDataInternal* roomData;
    RoomMemberDataInternalList members;
};

struct World {
    World* next;
    std::uint32_t worldId;
    std::uint32_t numOfLobby;
    std::uint32_t curNumOfTotalLobby;
    std::uint32_t maxNumOfTotalLobby;
    std::uint32_t curNumOfRoom;
    std::uint32_t maxNumOfRoom;
    std::uint32_t curNumOfTotalRoomMember;
    std::uint32_t numOfRoom;
};
static_assert(sizeof(World) == 0x28); // the game walks the list at this stride (sub_10be5c0)

struct GetWorldInfoListResponse {
    World* world;
    std::uint32_t worldNum;
    std::uint32_t pad;
};

struct RoomMemberUpdateInfo {
    RoomMemberDataInternal* roomMemberDataInternal;
    std::uint8_t eventCause;
    std::uint8_t pad[7];
    std::uint8_t optData[16];
    std::uint64_t optDataLen;
};

struct RequestOptParam {
    void* cbFunc;
    void* cbFuncArg;
    std::uint32_t timeout;
    std::uint16_t appReqId;
    std::uint16_t pad;
};

// RoomUpdateInfo, the data of KICKEDOUT and ROOM_DESTROYED: the game reads optData only
// when it is 4 bytes long - the reason the kicking host passed (sub_10c3da0 -> sub_1098070).
struct RoomUpdateInfo {
    std::uint8_t eventCause;
    std::uint8_t pad[3];
    std::int32_t errorCode;
    std::uint8_t optData[16];
    std::uint64_t optDataLen;
};
static_assert(offsetof(RoomUpdateInfo, optData) == 8 &&
              offsetof(RoomUpdateInfo, optDataLen) == 0x18);

// KickoutRoomMemberRequest: roomId, the member, blockKickFlag, optData.
struct KickoutRequest {
    std::uint64_t roomId;
    std::uint16_t memberId;
    std::uint8_t blockKickFlag;
    std::uint8_t pad[5];
    std::uint8_t optData[16];
    std::uint64_t optDataLen;
};
static_assert(offsetof(KickoutRequest, optData) == 0x10 &&
              offsetof(KickoutRequest, optDataLen) == 0x20);

struct SignalingGetPingInfoResponse {
    std::uint16_t serverId;
    std::uint16_t pad;
    std::uint32_t worldId;
    std::uint64_t roomId;
    std::uint32_t rtt; // microseconds
    std::uint32_t pad2;
};

// Events (SceNpMatching2Event).
constexpr std::uint16_t kEvGetWorldInfoList = 0x0002;
constexpr std::uint16_t kEvSetRoomDataExternal = 0x0004;
constexpr std::uint16_t kEvCreateJoinRoom = 0x0101;
constexpr std::uint16_t kEvJoinRoom = 0x0102;
constexpr std::uint16_t kEvLeaveRoom = 0x0103;
constexpr std::uint16_t kEvKickoutRoomMember = 0x0104;
constexpr std::uint16_t kEvGrantRoomOwner = 0x0105;
constexpr std::uint16_t kEvSearchRoom = 0x0106;
constexpr std::uint16_t kEvSetRoomDataInternal = 0x0109;
constexpr std::uint16_t kEvSetRoomMemberDataInternal = 0x010B;
constexpr std::uint16_t kEvSignalingGetPingInfo = 0x0E01;
constexpr std::uint16_t kEvMemberJoined = 0x1101;
constexpr std::uint16_t kEvMemberLeft = 0x1102;
constexpr std::uint16_t kEvKickedOut = 0x1103;
constexpr std::uint16_t kEvRoomDestroyed = 0x1104;
constexpr std::uint16_t kEvSignalingDead = 0x5101;
constexpr std::uint16_t kEvSignalingEstablished = 0x5102;
constexpr std::uint16_t kEvContextStarted = 0x6F02;

// SceNpMatching2EventCause.
constexpr std::uint8_t kCauseLeave = 1, kCauseKickout = 2, kCauseServerOperation = 4,
                       kCauseMemberDisappeared = 5;

constexpr int kErrInvalidArg = static_cast<int>(0x80550C03);
constexpr int kErrContext = static_cast<int>(0x80550C06);
constexpr int kErrConnectionFailed = static_cast<int>(0x80550C0B); // the game reports it
constexpr int kErrServerNotAvailable = static_cast<int>(0x80550C28);
// The signaling DEAD event's error: the peer left, or we did.
constexpr int kErrTerminatedByPeer = static_cast<int>(0x80550E10);
constexpr int kErrTerminatedByMyself = static_cast<int>(0x80550E18);

// ---- state ----------------------------------------------------------------

// A response the game reads during its callback. sub_10c3210 copies the room id and walks
// the member list inside the call, so keeping the last few alive is generous.
struct Held {
    std::vector<std::uint8_t> bytes;
};

struct M2 {
    std::mutex mu;
    unsigned ctx_id = 0;
    bool started = false;
    void* ctx_cb = nullptr;
    void* ctx_arg = nullptr;
    void* room_cb = nullptr;
    void* room_arg = nullptr;
    void* sig_cb = nullptr;
    void* sig_arg = nullptr;
    void* lobby_cb = nullptr;
    void* lobby_arg = nullptr;
    RequestOptParam dflt{};
    std::atomic<unsigned> next_req{1};
    // the room
    std::string session_id;
    std::uint64_t room_id = 0;
    std::uint16_t member_id = 0;
    std::uint16_t owner_id = 0;
    std::uint16_t max_slot = 0;
    bool is_host = false;
    bool in_room = false;
    // The room a JoinRoom is on its way into: its member events can come in on the poller
    // before the answer is in.
    std::uint64_t joining_room = 0;
    std::int64_t room_since_ms = 0;
    // LeaveRoom, deferred until the session ends
    bool leave_pending = false;
    unsigned leave_req = 0;
    RequestOptParam leave_opt{};
    std::vector<std::unique_ptr<Held>> held;
    std::string npid; // our handle
};
M2 g;

bool Trace() {
    return Cfg().trace;
}

void Hold(std::unique_ptr<Held> held) {
    g.held.push_back(std::move(held));
    if (g.held.size() > 8) g.held.erase(g.held.begin());
}

// Never trust a null callback: the game registers all of them, but a scheduled event can
// outlive a DestroyContext.
void FireRequest(void* fn, void* arg, unsigned req, std::uint16_t event, int error, void* data) {
    if (!fn) return;
    if (Trace() || event != kEvGetWorldInfoList) {
        Log("np: REQUEST_CB event 0x%x req %u error 0x%x", event, req, static_cast<unsigned>(error));
    }
    CallGuest(fn, g.ctx_id, req, event, error, GuestArg(data), GuestArg(arg));
}

// A callback read under g.mu and made once it is released. The game's handlers take their
// own locks (the SessionOwner lock) and call back into sceNpMatching2, whose entry points
// take g.mu: made under it, a callback could deadlock against a game thread holding the
// SessionOwner lock, or against itself.
struct M2Call {
    void* fn = nullptr;
    std::int64_t a[6] = {};
    void Run() const {
        if (fn) CallGuest(fn, a[0], a[1], a[2], a[3], a[4], a[5]);
    }
};
M2Call RequestCall(void* fn, void* arg, unsigned req, std::uint16_t event, int error, void* data) {
    if (!fn) return {};
    if (Trace() || event != kEvGetWorldInfoList) {
        Log("np: REQUEST_CB event 0x%x req %u error 0x%x", event, req, static_cast<unsigned>(error));
    }
    return {fn,
            {static_cast<std::int64_t>(g.ctx_id), static_cast<std::int64_t>(req),
             static_cast<std::int64_t>(event), error, GuestArg(data), GuestArg(arg)}};
}
M2Call ContextCall(std::uint16_t event, int cause, int error) {
    if (!g.ctx_cb) return {};
    Log("np: CONTEXT_CB event 0x%x", event);
    return {g.ctx_cb,
            {static_cast<std::int64_t>(g.ctx_id), static_cast<std::int64_t>(event), cause, error,
             GuestArg(g.ctx_arg), 0}};
}
M2Call RoomEventCall(std::uint16_t event, void* data, std::uint64_t room_id) {
    if (!g.room_cb) return {};
    Log("np: ROOM_EVENT_CB event 0x%x room %llu", event, static_cast<unsigned long long>(room_id));
    return {g.room_cb,
            {static_cast<std::int64_t>(g.ctx_id), static_cast<std::int64_t>(room_id),
             static_cast<std::int64_t>(event), GuestArg(data), GuestArg(g.room_arg), 0}};
}
M2Call RoomEventCall(std::uint16_t event, void* data) {
    return RoomEventCall(event, data, g.room_id);
}
void FireSignaling(std::uint16_t member, std::uint16_t event, int error, std::uint64_t room_id) {
    if (!g.sig_cb) return;
    if (Trace()) Log("np: SIGNALING_CB event 0x%x member %u", event, member);
    CallGuest(g.sig_cb, g.ctx_id, static_cast<std::int64_t>(room_id), member, event, error,
              GuestArg(g.sig_arg));
}
void FireSignaling(std::uint16_t member, std::uint16_t event, int error) {
    FireSignaling(member, event, error, g.room_id);
}

// The room's real members (provisional 0xff00+ entries are the same people, resolved before
// they joined).
std::vector<Peer> RoomPeers() {
    std::vector<Peer> v;
    for (const Peer& p : PeersAll()) {
        if (p.member_id < 0xff00) v.push_back(p);
    }
    return v;
}

// The ESTABLISHED event for every member the game must see before signalingPoll runs
// (ConnObj+0xd8), self included.
void ScheduleEstablishedForAll(int delay_ms) {
    std::vector<std::uint16_t> ids;
    for (const Peer& p : RoomPeers()) ids.push_back(p.member_id);
    ids.push_back(g.member_id);
    DispatchAfter(delay_ms, Prio::Signaling, [ids] {
        for (const std::uint16_t m : ids) FireSignaling(m, kEvSignalingEstablished, 0);
    });
}

// Members gone for good: the signaling DEAD for each first, then the room event, as the SDK
// delivers them (DEAD 0x5101 before MEMBER_LEFT).
void ScheduleDead(const std::vector<std::uint16_t>& ids, int error, std::uint64_t room_id,
                  int delay_ms) {
    if (ids.empty()) return;
    DispatchAfter(delay_ms, Prio::Signaling, [ids, error, room_id] {
        for (const std::uint16_t m : ids) FireSignaling(m, kEvSignalingDead, error, room_id);
    });
}

// Builds a RoomDataInternal + member list (self + every peer) into one Held.
CreateJoinRoomResponse* BuildRoomResponse(std::uint64_t room_id, std::uint16_t self_id,
                                          std::uint16_t owner_id, std::uint16_t max_slot) {
    const std::vector<Peer> peers = RoomPeers();
    const std::size_t n = peers.size() + 1;
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(CreateJoinRoomResponse) + sizeof(RoomDataInternal) +
                           n * sizeof(RoomMemberDataInternal),
                       0);
    auto* resp = reinterpret_cast<CreateJoinRoomResponse*>(held->bytes.data());
    auto* room = reinterpret_cast<RoomDataInternal*>(resp + 1);
    auto* members = reinterpret_cast<RoomMemberDataInternal*>(room + 1);
    room->maxSlot = max_slot ? max_slot : 5;
    room->publicSlots = room->maxSlot;
    room->openPublicSlots = static_cast<std::uint16_t>(room->maxSlot > n ? room->maxSlot - n : 0);
    room->serverId = 1;
    room->worldId = 1;
    room->lobbyId = 1;
    room->roomId = room_id;
    std::uint64_t joined = 0;
    std::size_t i = 0;
    const auto put = [&](std::uint16_t member_id, const std::string& online) {
        RoomMemberDataInternal& m = members[i];
        m.next = i + 1 < n ? &members[i + 1] : nullptr;
        NpFillNpId(m.npId, online.c_str());
        m.memberId = member_id;
        m.natType = 1;
        if (member_id && member_id <= 64) joined |= 1ull << (member_id - 1);
        if (member_id == self_id) resp->members.me = &m;
        if (member_id == owner_id) {
            resp->members.owner = &m;
            m.flagAttr |= 0x80000000u; // SCE_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER
        }
        ++i;
    };
    put(self_id, OnlineId());
    for (const Peer& p : peers) put(p.member_id, p.online_id);
    room->joinedSlotMask = joined;
    resp->roomData = room;
    resp->members.members = members;
    resp->members.membersNum = n;
    if (!resp->members.owner) resp->members.owner = resp->members.me;
    Hold(std::move(held));
    return resp;
}

RoomMemberUpdateInfo* BuildMemberUpdate(const Peer& p, std::uint8_t cause) {
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(RoomMemberUpdateInfo) + sizeof(RoomMemberDataInternal), 0);
    auto* info = reinterpret_cast<RoomMemberUpdateInfo*>(held->bytes.data());
    auto* m = reinterpret_cast<RoomMemberDataInternal*>(info + 1);
    NpFillNpId(m->npId, p.online_id.c_str());
    m->memberId = p.member_id;
    m->natType = 1;
    if (p.member_id == g.owner_id) m->flagAttr |= 0x80000000u;
    info->roomMemberDataInternal = m;
    info->eventCause = cause;
    Hold(std::move(held));
    return info;
}

RoomUpdateInfo* BuildRoomUpdate(std::uint8_t cause, const std::uint8_t* opt, std::size_t opt_len) {
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(RoomUpdateInfo), 0);
    auto* info = reinterpret_cast<RoomUpdateInfo*>(held->bytes.data());
    info->eventCause = cause;
    opt_len = opt_len > sizeof(info->optData) ? sizeof(info->optData) : opt_len;
    if (opt && opt_len) std::memcpy(info->optData, opt, opt_len);
    info->optDataLen = opt_len;
    Hold(std::move(held));
    return info;
}

RequestOptParam OptOrDefault(const RequestOptParam* opt) {
    return opt && opt->cbFunc ? *opt : g.dflt;
}

void ForgetRoomLocked() {
    g.in_room = false;
    g.session_id.clear();
    g.room_id = 0;
    g.owner_id = 0;
    g.max_slot = 0;
    PeersClear();
    RoomClear();
    NpSignalingReset();
}

// Our LeaveRoom (or the context going away): out of the server's room, DEAD for every
// member, then 0x103 at +300 ms.
void EndSessionLocked(const char* why) {
    if (!g.in_room && !g.leave_pending) return;
    Log("np: session over (%s): room %llu", why, static_cast<unsigned long long>(g.room_id));
    if (!g.session_id.empty()) {
        const std::string sid = g.session_id;
        const int mid = g.member_id;
        Json reply;
        std::string err;
        ServerLeaveRoom(sid, mid, reply, err);
    }
    std::vector<std::uint16_t> ids;
    for (const Peer& p : RoomPeers()) ids.push_back(p.member_id);
    ScheduleDead(ids, kErrTerminatedByMyself, g.room_id, 0);
    if (g.leave_pending) {
        const unsigned req = g.leave_req;
        const RequestOptParam opt = g.leave_opt;
        g.leave_pending = false;
        DispatchAfter(300, Prio::Request, [req, opt] {
            FireRequest(opt.cbFunc, opt.cbFuncArg, req, kEvLeaveRoom, 0, nullptr);
        });
    }
    ForgetRoomLocked();
}

// The server ended our membership (the host left or timed out, we were kicked, the server
// lost the room): the game still holds the room until it hears so - KICKEDOUT or
// ROOM_DESTROYED, after a DEAD per member.
void RoomGoneLocked(std::uint16_t event, std::uint8_t cause, const std::uint8_t* opt,
                    std::size_t opt_len, const std::string& why) {
    if (!g.in_room) return;
    const std::uint64_t room = g.room_id;
    Log("np: room %llu gone (%s): the game hears 0x%x", static_cast<unsigned long long>(room),
        why.c_str(), event);
    std::vector<std::uint16_t> ids;
    for (const Peer& p : RoomPeers()) {
        ids.push_back(p.member_id);
        NpSignalingPeerDead(p.online_id);
    }
    ScheduleDead(ids, kErrTerminatedByPeer, room, 0);
    RoomUpdateInfo* info = BuildRoomUpdate(cause, opt, opt_len);
    DispatchAfter(100, Prio::RoomEvent, [event, info, room] {
        M2Call c;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            c = RoomEventCall(event, info, room);
        }
        c.Run();
    });
    ForgetRoomLocked();
}

// The server drops a member whose heartbeat is 15 s old (its HEARTBEAT_TIMEOUT); one every
// 5 s while we are in a room. A server that answers we are not in the room any more (it
// dropped the room: a restart, our own heartbeat lapsing) ends the room for the game.
void HeartbeatLoop() {
    std::string sid;
    int mid = 0;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        if (g.in_room && !g.session_id.empty()) {
            sid = g.session_id;
            mid = g.member_id;
        }
    }
    if (!sid.empty()) {
        const int r = ServerHeartbeat(sid, mid);
        if (r < 0 && Trace()) Log("np: heartbeat not acknowledged");
        if (r == 0) {
            std::lock_guard<std::mutex> lk(g.mu);
            if (g.in_room && g.session_id == sid) {
                RoomGoneLocked(kEvRoomDestroyed, kCauseServerOperation, nullptr, 0,
                               "the server has no room for us");
            }
        }
    }
    DispatchAfter(5000, Prio::Request, HeartbeatLoop);
}

std::vector<std::uint8_t> B64Decode(const std::string& in) {
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z') {
            v = ch - 'A';
        } else if (ch >= 'a' && ch <= 'z') {
            v = ch - 'a' + 26;
        } else if (ch >= '0' && ch <= '9') {
            v = ch - '0' + 52;
        } else if (ch == '+') {
            v = 62;
        } else if (ch == '/') {
            v = 63;
        } else {
            continue;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(acc >> bits));
        }
    }
    return out;
}

bool IsLoopback(std::uint32_t addr_nbo) {
    return (addr_nbo & 0xff) == 127;
}
bool ServerIsLocal() {
    const std::string base = NpServerBase();
    return base.find("://127.") != std::string::npos ||
           base.find("://localhost") != std::string::npos;
}

// ---- events from the server -----------------------------------------------

void OnServerEvent(const Json& ev);

// An event about the room a JoinRoom is still answering for: handled again once the answer
// is in (the poller can be quicker than the join's reply).
bool DeferForJoinLocked(const Json& ev) {
    const auto room = static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0));
    if (g.in_room || !room || room != g.joining_room) return false;
    const int tries = static_cast<int>(IntOf(ev, "_Deferred", 0));
    if (tries >= 20) return false;
    Json again = ev;
    again["_Deferred"] = tries + 1;
    DispatchAfter(250, Prio::RoomEvent, [again] { OnServerEvent(again); });
    return true;
}

void OnServerEvent(const Json& ev) {
    const std::string name = StrOf(ev, "Name");
    if (name == "guest_invite") {
        std::lock_guard<std::mutex> lk(g.mu);
        const std::uint64_t room = static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0));
        const std::string host_online = StrOf(ev, "HostOnlineId");
        // The host's addresses as the server knows them: the peer table takes them now so
        // JoinRoom's answer only confirms, and the punch opens our NAT toward the host
        // before its first datagram. A server before 2026-10-06 sent the host's
        // self-reported 127.0.0.1 here, which pointed us at ourselves: such an invite seeds
        // nothing, and the host is resolved when the game asks for it.
        Json rec = Json::object();
        rec["MemberId"] = 1;
        rec["OnlineId"] = host_online;
        rec["Addr"] = StrOf(ev, "HostAddr");
        rec["Port"] = IntOf(ev, "HostPort", 0);
        rec["LocalAddr"] = StrOf(ev, "HostLocalAddr");
        rec["LocalPort"] = IntOf(ev, "HostLocalPort", 0);
        rec["MappedAddr"] = StrOf(ev, "HostMappedAddr");
        rec["MappedPort"] = IntOf(ev, "HostMappedPort", 0);
        Json one = Json::array();
        one.push_back(rec);
        Peer before;
        const bool had = PeersGet(1, &before);
        PeersFromMembers(one);
        Peer seeded;
        if (PeersGet(1, &seeded) && IsLoopback(seeded.addr) && !ServerIsLocal()) {
            if (had) {
                PeersUpsert(before);
            } else {
                PeersErase(1);
            }
            Log("np: guest_invite: the host's address is loopback (an older server); not seeded");
        }
        // The host's game builds the invite itself (its type-1 item, with the session blob)
        // and sends it over its P2P socket once the signaling connection to this guest is
        // active; this event only seeds the host's address.
        Log("np: guest_invite: room %llu from %s (%s:%lld)", static_cast<unsigned long long>(room),
            host_online.c_str(), StrOf(ev, "HostAddr").c_str(), IntOf(ev, "HostPort", 0));
        return;
    }
    if (name == "room_member_joined") {
        std::lock_guard<std::mutex> lk(g.mu);
        const auto mid = static_cast<std::uint16_t>(IntOf(ev, "MemberId", 0));
        if (DeferForJoinLocked(ev)) return;
        if (!g.in_room || !mid || mid == g.member_id) return;
        if (static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0)) != g.room_id) return;
        // A member our JoinRoom answer already listed (a server before 2026-10-06 announced
        // the existing guests to a joiner again): the address is news, the member is not -
        // a second MEMBER_JOINED would make the game build it twice.
        Peer known;
        const bool already = PeersGet(mid, &known) && known.online_id == StrOf(ev, "OnlineId");
        Json one = Json::array();
        one.push_back(ev);
        if (PeersFromMembers(one) == 0) return;
        Peer p;
        if (!PeersGet(mid, &p)) return;
        NpSignalingPeerKnown(p.member_id);
        if (already) {
            if (Trace()) {
                Log("np: member %u (%s) already known; address refreshed", mid, p.online_id.c_str());
            }
            return;
        }
        Log("np: member %u (%s) joined room %llu", p.member_id, p.online_id.c_str(),
            static_cast<unsigned long long>(g.room_id));
        DispatchAfter(300, Prio::RoomEvent, [p] {
            M2Call c;
            {
                std::lock_guard<std::mutex> lk2(g.mu);
                c = RoomEventCall(kEvMemberJoined, BuildMemberUpdate(p, 0));
            }
            c.Run();
        });
        DispatchAfter(800, Prio::Signaling, [mid] {
            FireSignaling(mid, kEvSignalingEstablished, 0);
            FireSignaling(g.member_id, kEvSignalingEstablished, 0);
        });
        return;
    }
    if (name == "room_member_left") {
        std::lock_guard<std::mutex> lk(g.mu);
        const auto mid = static_cast<std::uint16_t>(IntOf(ev, "MemberId", 0));
        if (DeferForJoinLocked(ev)) return;
        if (static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0)) != g.room_id) return;
        Peer p;
        if (!PeersGet(mid, &p)) return;
        const std::string reason = StrOf(ev, "Reason");
        const std::uint8_t cause = reason == "kicked"                           ? kCauseKickout
                                   : (reason == "leave_room" || reason.empty()) ? kCauseLeave
                                                                                : kCauseMemberDisappeared;
        Log("np: member %u (%s) left (%s)", mid, p.online_id.c_str(), reason.c_str());
        // DEAD for that member, then MEMBER_LEFT; the room stays ours.
        ScheduleDead({mid}, kErrTerminatedByPeer, g.room_id, 0);
        DispatchAfter(100, Prio::RoomEvent, [p, cause] {
            M2Call c;
            {
                std::lock_guard<std::mutex> lk2(g.mu);
                c = RoomEventCall(kEvMemberLeft, BuildMemberUpdate(p, cause));
            }
            c.Run();
        });
        NpSignalingPeerDead(p.online_id);
        PeersErase(mid);
        // Its provisional entry (resolved before it joined) goes too: a return resolves it
        // afresh.
        for (const Peer& q : PeersAll()) {
            if (q.member_id >= 0xff00 && q.online_id == p.online_id) PeersErase(q.member_id);
        }
        return;
    }
    if (name == "peer_deactivated") {
        // The server timed the peer out; its connection is dead for us too.
        std::string who = StrOf(ev, "OnlineId");
        if (who.empty()) who = StrOf(ev, "PeerOnlineId");
        if (!who.empty()) NpSignalingPeerDead(who);
        return;
    }
    if (name == "room_closed") {
        std::lock_guard<std::mutex> lk(g.mu);
        const auto room = static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0));
        if (room && room != g.room_id) return;
        const std::string reason = StrOf(ev, "Reason");
        RoomGoneLocked(kEvRoomDestroyed, reason == "host_left" ? kCauseLeave : kCauseServerOperation,
                       nullptr, 0, "room closed: " + reason);
        return;
    }
    if (name == "room_member_kicked") {
        std::lock_guard<std::mutex> lk(g.mu);
        const auto room = static_cast<std::uint64_t>(IntOf(ev, "RoomId", 0));
        const auto mid = static_cast<std::uint16_t>(IntOf(ev, "MemberId", 0));
        if (room != g.room_id || mid != g.member_id) return;
        const std::vector<std::uint8_t> opt = B64Decode(StrOf(ev, "OptData"));
        RoomGoneLocked(kEvKickedOut, kCauseKickout, opt.data(), opt.size(), "kicked by the host");
        return;
    }
    if (Trace()) Log("np: event %s ignored", name.c_str());
}

// ---- the imports ----------------------------------------------------------

int PS4_SYSV_ABI m2_initialize(const void*) {
    Log("sceNpMatching2Initialize");
    return 0;
}
int PS4_SYSV_ABI m2_terminate() {
    Log("sceNpMatching2Terminate");
    std::lock_guard<std::mutex> lk(g.mu);
    PollerStop();
    g.started = false;
    return 0;
}
// Bloodborne's two-argument form: (const CreateContextParam*, ctxId*). The param's first
// field is the SceNpId pointer.
int PS4_SYSV_ABI m2_create_context(const void* param, unsigned* ctx) {
    if (!ctx) return kErrInvalidArg;
    std::lock_guard<std::mutex> lk(g.mu);
    if (param) {
        const void* npid = nullptr;
        std::memcpy(&npid, param, sizeof(npid));
        if (npid) g.npid = NpIdText(npid);
    }
    if (!g.ctx_id) g.ctx_id = 1;
    *ctx = g.ctx_id;
    Log("sceNpMatching2CreateContext (%s) -> %u", g.npid.c_str(), g.ctx_id);
    return 0;
}
int PS4_SYSV_ABI m2_register_context_cb(void* fn, void* arg) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.ctx_cb = fn;
    g.ctx_arg = arg;
    return 0;
}
int PS4_SYSV_ABI m2_context_start(unsigned ctx, unsigned) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    if (g.started) return 0;
    g.started = true;
    Log("sceNpMatching2ContextStart -> server %s as %s%s", NpServerBase().c_str(),
        OnlineId().c_str(), AccountLoggedIn() ? " (account)" : "");
    Engine::SetSignalingGate();
    PollerStart(OnServerEvent);
    static bool heartbeat_started = false;
    if (!heartbeat_started) {
        heartbeat_started = true;
        DispatchAfter(5000, Prio::Request, HeartbeatLoop);
    }
    // Register with the server off the game's thread; the started event follows at the
    // contract's delay whether or not the server answered, since an unreachable server
    // should read as "online but alone", not a stuck boot.
    DispatchAfter(0, Prio::Context, [] {
        Json reply;
        std::string err;
        if (!ServerContextStart(reply, err)) {
            Log("np: context_start failed: %s", err.c_str());
        }
    });
    DispatchAfter(200, Prio::Context, [] {
        M2Call c;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            c = ContextCall(kEvContextStarted, 0, 0);
        }
        c.Run();
    });
    return 0;
}
int PS4_SYSV_ABI m2_context_stop(unsigned) {
    std::lock_guard<std::mutex> lk(g.mu);
    PollerStop();
    g.started = false;
    return 0;
}
int PS4_SYSV_ABI m2_destroy_context(unsigned) {
    std::lock_guard<std::mutex> lk(g.mu);
    PollerStop();
    EndSessionLocked("context destroyed");
    g.started = false;
    g.ctx_cb = g.room_cb = g.sig_cb = g.lobby_cb = nullptr;
    return 0;
}
int PS4_SYSV_ABI m2_get_server_id(unsigned ctx, std::uint16_t* id) {
    if (!id) return kErrInvalidArg;
    if (ctx != g.ctx_id) return kErrContext;
    *id = 1;
    return 0;
}
int PS4_SYSV_ABI m2_set_default_opt(unsigned, const RequestOptParam* opt) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (opt) g.dflt = *opt;
    return 0;
}
int PS4_SYSV_ABI m2_register_room_event_cb(unsigned, void* fn, void* arg) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.room_cb = fn;
    g.room_arg = arg;
    return 0;
}
int PS4_SYSV_ABI m2_register_signaling_cb(unsigned, void* fn, void* arg) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.sig_cb = fn;
    g.sig_arg = arg;
    return 0;
}
int PS4_SYSV_ABI m2_register_lobby_event_cb(unsigned, void* fn, void* arg) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.lobby_cb = fn;
    g.lobby_arg = arg;
    return 0;
}

int PS4_SYSV_ABI m2_get_world_info_list(unsigned ctx, const void*, const RequestOptParam* opt,
                                        unsigned* req_out) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = OptOrDefault(opt);
    DispatchAfter(100, Prio::Request, [req, o] {
        M2Call c;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            auto held = std::make_unique<Held>();
            held->bytes.assign(sizeof(GetWorldInfoListResponse) + sizeof(World), 0);
            auto* resp = reinterpret_cast<GetWorldInfoListResponse*>(held->bytes.data());
            auto* w = reinterpret_cast<World*>(resp + 1);
            w->worldId = 1;
            w->numOfLobby = 1;
            w->maxNumOfTotalLobby = 1;
            w->maxNumOfRoom = 1000;
            w->numOfRoom = 1000;
            resp->world = w;
            resp->worldNum = 1;
            Hold(std::move(held));
            c = RequestCall(o.cbFunc, o.cbFuncArg, req, kEvGetWorldInfoList, 0, resp);
        }
        c.Run();
    });
    return 0;
}

// CreateJoinRoomRequest: maxSlot u16 at +0, worldId u32 at +0xc.
int PS4_SYSV_ABI m2_create_join_room(unsigned ctx, const std::uint8_t* reqp,
                                     const RequestOptParam* opt, unsigned* req_out) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    std::uint16_t max_slot = 5; // Bloodborne asks 5: host, two cooperators, two invaders
    if (reqp) std::memcpy(&max_slot, reqp, 2);
    const RequestOptParam o = OptOrDefault(opt);
    Log("sceNpMatching2CreateJoinRoom req %u maxSlot %u%s", req, max_slot,
        g.in_room ? " (the game left its room without LeaveRoom; the server closes it)" : "");
    DispatchAfter(0, Prio::Request, [req, o, max_slot] {
        Json extra = Json::object();
        Engine::HostExtra(extra);
        Json reply;
        std::string err;
        std::uint64_t room = 0;
        std::string sid;
        int error = 0;
        if (ServerCreateRoom(max_slot, extra, reply, err) && IntOf(reply, "ResKind", -1) == 0) {
            room = static_cast<std::uint64_t>(IntOf(reply, "RoomId", 0));
            sid = StrOf(reply, "SessionId");
        } else {
            Log("np: create_room failed: %s",
                err.empty() ? reply.dump().substr(0, 200).c_str() : err.c_str());
            error = kErrConnectionFailed;
        }
        CreateJoinRoomResponse* resp = nullptr;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            if (!error) {
                g.session_id = sid;
                g.room_id = room;
                g.member_id = static_cast<std::uint16_t>(IntOf(reply, "MemberId", 1));
                g.owner_id = g.member_id;
                g.max_slot = static_cast<std::uint16_t>(IntOf(reply, "MaxMembers", max_slot));
                g.is_host = true;
                g.in_room = true;
                g.room_since_ms = NowMs();
                PeersClear();
                RoomSet(sid, g.member_id);
                resp = BuildRoomResponse(room, g.member_id, g.member_id, max_slot);
                Log("np: room %llu created, we are member %u",
                    static_cast<unsigned long long>(room), g.member_id);
            }
        }
        DispatchAfter(300, Prio::Request, [req, o, error, resp] {
            FireRequest(o.cbFunc, o.cbFuncArg, req, kEvCreateJoinRoom, error, resp);
        });
        if (!error) {
            std::lock_guard<std::mutex> lk2(g.mu);
            ScheduleEstablishedForAll(800);
        }
    });
    return 0;
}

// JoinRoomRequest: roomId u64 at +0.
int PS4_SYSV_ABI m2_join_room(unsigned ctx, const std::uint8_t* reqp, const RequestOptParam* opt,
                              unsigned* req_out) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    std::uint64_t room = 0;
    if (reqp) std::memcpy(&room, reqp, 8);
    const RequestOptParam o = OptOrDefault(opt);
    Log("sceNpMatching2JoinRoom req %u room %llu", req, static_cast<unsigned long long>(room));
    g.joining_room = room;
    DispatchAfter(0, Prio::Request, [req, o, room] {
        Json reply;
        std::string err;
        int error = 0;
        if (!(ServerJoinRoom(room, reply, err) && IntOf(reply, "ResKind", -1) == 0)) {
            Log("np: join_room failed: %s",
                err.empty() ? StrOf(reply, "Error", "refused").c_str() : err.c_str());
            error = kErrConnectionFailed;
        }
        CreateJoinRoomResponse* resp = nullptr;
        {
            std::lock_guard<std::mutex> lk2(g.mu);
            if (g.joining_room == room) g.joining_room = 0;
            if (!error) {
                g.session_id = StrOf(reply, "SessionId");
                g.room_id = room;
                g.member_id = static_cast<std::uint16_t>(IntOf(reply, "MemberId", 2));
                // The room's size and owner as the server has them (a server before
                // 2026-10-06 sent neither: the host, and the 5 the game asks for).
                g.owner_id = static_cast<std::uint16_t>(IntOf(reply, "OwnerMemberId", 1));
                g.max_slot = static_cast<std::uint16_t>(IntOf(reply, "MaxMembers", 5));
                g.is_host = false;
                g.in_room = true;
                g.room_since_ms = NowMs();
                // The member list is the room: entries from an invite or an earlier room go
                // (provisional ones stay for their connections).
                for (const Peer& p : PeersAll()) {
                    if (p.member_id < 0xff00) PeersErase(p.member_id);
                }
                if (reply.is_object()) {
                    const auto members = reply.find("Members");
                    if (members != reply.end()) PeersFromMembers(*members);
                }
                for (const Peer& p : PeersAll()) NpSignalingPeerKnown(p.member_id);
                RoomSet(g.session_id, g.member_id);
                resp = BuildRoomResponse(room, g.member_id, g.owner_id, g.max_slot);
                Log("np: joined room %llu as member %u with %zu peers",
                    static_cast<unsigned long long>(room), g.member_id, RoomPeers().size());
            }
        }
        // The host's rules, when the server says them (HostRuleset) and they are not ours:
        // bbhost's plugins rewrite the world the guest is about to load. This port has no
        // such plugins; it plays the world as its own files have it.
        if (!error) {
            const std::string host_rules = StrOf(reply, "HostRuleset");
            if (!host_rules.empty() && host_rules != Cfg().ruleset) {
                Log("np: the host plays by other rules (%s); this port cannot adopt them",
                    host_rules.substr(0, 96).c_str());
            }
        }
        DispatchAfter(300, Prio::Request, [req, o, error, resp] {
            FireRequest(o.cbFunc, o.cbFuncArg, req, kEvJoinRoom, error, resp);
        });
        if (!error) {
            std::lock_guard<std::mutex> lk2(g.mu);
            ScheduleEstablishedForAll(500);
        }
    });
    return 0;
}

int PS4_SYSV_ABI m2_leave_room(unsigned ctx, const void*, const RequestOptParam* opt,
                               unsigned* req_out) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = OptOrDefault(opt);
    // The game calls this only when its session is over - the guest gone, or the host
    // giving up. So: leave the server's room now, answer 0x103 at +300 ms, and log if it
    // ever comes within 2 s of the room.
    Log("sceNpMatching2LeaveRoom req %u%s", req,
        g.in_room && NowMs() - g.room_since_ms < 2000 ? " (2 s after the room was made!)" : "");
    g.leave_pending = true;
    g.leave_req = req;
    g.leave_opt = o;
    EndSessionLocked("LeaveRoom");
    return 0;
}

// (ctx, roomId, memberId, status*, SceNetInAddr*, SceInPort_t*): the three values.
int PS4_SYSV_ABI m2_signaling_get_connection_status(unsigned ctx, std::uint64_t room,
                                                    std::uint16_t member, int* status,
                                                    std::uint32_t* addr, std::uint16_t* port) {
    if (ctx != g.ctx_id) return kErrContext;
    Peer p;
    const bool known = PeersGet(member, &p) && p.addr != 0;
    if (status) *status = known ? 2 : 0;
    if (addr) *addr = known ? p.addr : 0;
    if (port) *port = known ? static_cast<std::uint16_t>((p.port << 8) | (p.port >> 8)) : 0;
    if (Trace()) {
        Log("sceNpMatching2SignalingGetConnectionStatus room %llu member %u -> %d",
            static_cast<unsigned long long>(room), member, known ? 2 : 0);
    }
    return 0;
}

// The requests the summon path does not need still answer, as the SDK's always do: the
// game's room object marks a request pending until its callback (SessionOwner+0x458) and
// runs nothing else meanwhile.
int AnswerRequest(unsigned ctx, const RequestOptParam* opt, unsigned* req_out, std::uint16_t event,
                  int error, const char* what) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = OptOrDefault(opt);
    Log("sceNpMatching2%s req %u: answered 0x%x%s", what, req, event,
        error ? " with an error" : "");
    DispatchAfter(100, Prio::Request, [o, req, event, error] {
        FireRequest(o.cbFunc, o.cbFuncArg, req, event, error, nullptr);
    });
    return 0;
}

// The host sends a member away (sub_10c5b70: a 4-byte reason in optData, which the kicked
// game reads from KICKEDOUT). The server takes the member out and tells everyone, the
// kicked one included; without the server the room hears it here.
int PS4_SYSV_ABI m2_kickout(unsigned ctx, const KickoutRequest* rq, const RequestOptParam* opt,
                            unsigned* req_out) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    if (!rq) return kErrInvalidArg;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = OptOrDefault(opt);
    const KickoutRequest r = *rq;
    Log("sceNpMatching2KickoutRoomMember req %u room %llu member %u (%llu-byte reason)", req,
        static_cast<unsigned long long>(r.roomId), r.memberId,
        static_cast<unsigned long long>(r.optDataLen));
    const std::string sid = g.session_id;
    const int me = g.member_id;
    DispatchAfter(0, Prio::Request, [o, req, r, sid, me] {
        std::string err;
        const std::size_t n = r.optDataLen > sizeof(r.optData)
                                  ? sizeof(r.optData)
                                  : static_cast<std::size_t>(r.optDataLen);
        const bool sent = !sid.empty() && ServerKickMember(sid, r.memberId, me, r.optData, n, err);
        if (!sent) {
            Log("np: kick of member %u not sent (%s); the room hears it here", r.memberId,
                sid.empty() ? "no room" : err.c_str());
            Json ev = Json::object();
            ev["Name"] = "room_member_left";
            {
                std::lock_guard<std::mutex> lk2(g.mu);
                ev["RoomId"] = static_cast<long long>(g.room_id);
            }
            ev["MemberId"] = static_cast<int>(r.memberId);
            ev["Reason"] = "kicked";
            OnServerEvent(ev);
        }
        DispatchAfter(100, Prio::Request, [o, req] {
            FireRequest(o.cbFunc, o.cbFuncArg, req, kEvKickoutRoomMember, 0, nullptr);
        });
    });
    return 0;
}
int PS4_SYSV_ABI m2_grant_room_owner(unsigned ctx, const void*, const RequestOptParam* opt,
                                     unsigned* req_out) {
    return AnswerRequest(ctx, opt, req_out, kEvGrantRoomOwner, 0, "GrantRoomOwner");
}
int PS4_SYSV_ABI m2_set_room_data_internal(unsigned ctx, const void*, const RequestOptParam* opt,
                                           unsigned* req_out) {
    return AnswerRequest(ctx, opt, req_out, kEvSetRoomDataInternal, 0, "SetRoomDataInternal");
}
int PS4_SYSV_ABI m2_set_room_data_external(unsigned ctx, const void*, const RequestOptParam* opt,
                                           unsigned* req_out) {
    return AnswerRequest(ctx, opt, req_out, kEvSetRoomDataExternal, 0, "SetRoomDataExternal");
}
int PS4_SYSV_ABI m2_set_room_member_data_internal(unsigned ctx, const void*,
                                                  const RequestOptParam* opt, unsigned* req_out) {
    return AnswerRequest(ctx, opt, req_out, kEvSetRoomMemberDataInternal, 0,
                         "SetRoomMemberDataInternal");
}
// No room list here: summons go through the game's own sign server. The game's handler
// reads an error as "nothing found" (sub_10be2a0 posts 0x2a).
int PS4_SYSV_ABI m2_search_room(unsigned ctx, const void*, const RequestOptParam* opt,
                                unsigned* req_out) {
    return AnswerRequest(ctx, opt, req_out, kEvSearchRoom, kErrServerNotAvailable, "SearchRoom");
}
// (ctx, {roomId}, opt, reqId): the round trip through the relay, from the keepalive's STUN
// exchange (ours to the server, twice for a peer's).
int PS4_SYSV_ABI m2_signaling_get_ping_info(unsigned ctx, const std::uint64_t* rq,
                                            const RequestOptParam* opt, unsigned* req_out) {
    std::lock_guard<std::mutex> lk(g.mu);
    if (ctx != g.ctx_id) return kErrContext;
    const unsigned req = g.next_req++;
    if (req_out) *req_out = req;
    const RequestOptParam o = OptOrDefault(opt);
    auto held = std::make_unique<Held>();
    held->bytes.assign(sizeof(SignalingGetPingInfoResponse), 0);
    auto* resp = reinterpret_cast<SignalingGetPingInfoResponse*>(held->bytes.data());
    resp->serverId = 1;
    resp->worldId = 1;
    resp->roomId = rq ? *rq : g.room_id;
    const int rtt = StunRttUs();
    resp->rtt = static_cast<std::uint32_t>(rtt > 0 ? 2 * rtt : 60000);
    Hold(std::move(held));
    DispatchAfter(50, Prio::Request, [o, req, resp] {
        FireRequest(o.cbFunc, o.cbFuncArg, req, kEvSignalingGetPingInfo, 0, resp);
    });
    return 0;
}
// Lobbies: never called by this game; bound so a call is visible.
int PS4_SYSV_ABI m2_unused(unsigned, const void*, const RequestOptParam*, unsigned* req_out) {
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 4) Log("np: an unused NpMatching2 lobby request was called");
    if (req_out) *req_out = g.next_req++;
    return 0;
}

} // namespace

bool NpContextStarted() {
    std::lock_guard<std::mutex> lk(g.mu);
    return g.started;
}

void RegisterNpMatching2(Exports& t) {
    HD_EXPORT(t, "sceNpMatching2Initialize", m2_initialize);
    HD_EXPORT(t, "sceNpMatching2Terminate", m2_terminate);
    HD_EXPORT(t, "sceNpMatching2CreateContext", m2_create_context);
    HD_EXPORT(t, "sceNpMatching2RegisterContextCallback", m2_register_context_cb);
    HD_EXPORT(t, "sceNpMatching2ContextStart", m2_context_start);
    HD_EXPORT(t, "sceNpMatching2ContextStop", m2_context_stop);
    HD_EXPORT(t, "sceNpMatching2DestroyContext", m2_destroy_context);
    HD_EXPORT(t, "sceNpMatching2GetServerId", m2_get_server_id);
    HD_EXPORT(t, "sceNpMatching2SetDefaultRequestOptParam", m2_set_default_opt);
    HD_EXPORT(t, "sceNpMatching2RegisterRoomEventCallback", m2_register_room_event_cb);
    HD_EXPORT(t, "sceNpMatching2RegisterSignalingCallback", m2_register_signaling_cb);
    HD_EXPORT(t, "sceNpMatching2RegisterLobbyEventCallback", m2_register_lobby_event_cb);
    HD_EXPORT(t, "sceNpMatching2GetWorldInfoList", m2_get_world_info_list);
    HD_EXPORT(t, "sceNpMatching2CreateJoinRoom", m2_create_join_room);
    HD_EXPORT(t, "sceNpMatching2JoinRoom", m2_join_room);
    HD_EXPORT(t, "sceNpMatching2LeaveRoom", m2_leave_room);
    HD_EXPORT(t, "sceNpMatching2SignalingGetConnectionStatus", m2_signaling_get_connection_status);
    HD_EXPORT(t, "sceNpMatching2SearchRoom", m2_search_room);
    HD_EXPORT(t, "sceNpMatching2JoinLobby", m2_unused);
    HD_EXPORT(t, "sceNpMatching2LeaveLobby", m2_unused);
    HD_EXPORT(t, "sceNpMatching2GetLobbyInfoList", m2_unused);
    HD_EXPORT(t, "sceNpMatching2KickoutRoomMember", m2_kickout);
    HD_EXPORT(t, "sceNpMatching2GrantRoomOwner", m2_grant_room_owner);
    HD_EXPORT(t, "sceNpMatching2SetRoomDataInternal", m2_set_room_data_internal);
    HD_EXPORT(t, "sceNpMatching2SetRoomDataExternal", m2_set_room_data_external);
    HD_EXPORT(t, "sceNpMatching2SetRoomMemberDataInternal", m2_set_room_member_data_internal);
    HD_EXPORT(t, "sceNpMatching2SignalingGetPingInfo", m2_signaling_get_ping_info);
}

} // namespace Hd
