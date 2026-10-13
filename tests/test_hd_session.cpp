// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream backend (gpu/bbnet/hd) without the game: this program plays the game's
// part of a summon in miniature, through the same imports the game calls and with callbacks
// that read the SDK's structures at the offsets the game reads them. Two copies of it - a
// host and a guest - meet through tests/hd_mock_server.py (tests/test_hd_session.py runs
// them):
//
//   host:  starts its context, makes its room, activates a signaling connection to the
//          guest (resolved by the server before the guest is a member), sends it an invite
//          datagram over the P2P port until it joins, exchanges datagrams with it, then
//          sees it leave (or kicks it) and leaves the room
//   guest: starts its context, takes the invite datagram, joins the room, exchanges
//          datagrams, leaves (or is kicked)
//
// Build: ninja -C out/gpu hd-session-test
// Usage: hd-session-test host|guest <peer online id> [--kick] [--hold-ms N]
//        hd-session-test signin - [--hold-ms N]   (the sign-in alone: context start and polls)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/types.h"
#include "hd/hd.h"
#include "hd/hd_p2p.h"

// The runtime's guest threads (src/runtime_thread.c); here a host thread does.
extern "C" std::int32_t runtime_thread_spawn(void** out, void* PS4_SYSV_ABI (*entry)(void*),
                                             void* argument, std::uint64_t, const char*) {
    std::thread([entry, argument] { entry(argument); }).detach();
    if (out) *out = nullptr;
    return 0;
}

namespace {

[[noreturn]] void Die(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::printf("FAIL: %s\n", line);
    std::fflush(stdout);
    std::_Exit(1);
}

// ---- what the callbacks saw ------------------------------------------------------------------

struct Member {
    unsigned id = 0;
    std::string name;
    bool owner = false;
};
struct Event {
    std::string kind;      // context, request, room, signaling, npsignaling, datagram
    int event = 0;         // the SDK's event number (or 0 for a datagram)
    int error = 0;
    std::uint64_t room = 0;
    unsigned member = 0;   // signaling: the member; npsignaling: the connection
    unsigned me = 0, owner = 0, max_slot = 0, cause = 0;
    std::vector<Member> members;
    std::vector<std::uint8_t> opt;
    std::string text;      // datagram: "KIND who arg"
    std::uint32_t addr = 0;
    std::uint16_t port = 0, vport = 0;
    bool taken = false;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<Event> g_events;
std::string g_me;
std::atomic<int> g_pings_answered{0};

// HD_TEST_TRACE=1: every callback and datagram as it arrives, with the time since the start.
const bool g_trace = std::getenv("HD_TEST_TRACE") != nullptr;
const auto g_start = std::chrono::steady_clock::now();
long long Ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 g_start)
        .count();
}

void Post(Event e) {
    if (g_trace && e.kind != "datagram") {
        std::printf("%6lld ms %s: %s 0x%x member %u error 0x%x\n", Ms(), g_me.c_str(), e.kind.c_str(),
                    static_cast<unsigned>(e.event), e.member, static_cast<unsigned>(e.error));
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_events.push_back(std::move(e));
    g_cv.notify_all();
}

template <typename Pred>
bool Take(Pred pred, int timeout_ms, Event* out) {
    std::unique_lock<std::mutex> lk(g_mu);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        for (Event& e : g_events) {
            if (!e.taken && pred(e)) {
                e.taken = true;
                if (out) *out = e;
                return true;
            }
        }
        if (g_cv.wait_until(lk, deadline) == std::cv_status::timeout) return false;
    }
}

Event Expect(const char* kind, int event, int timeout_ms = 15000, int member = -1) {
    Event e;
    const std::string k = kind;
    if (!Take([&](const Event& x) {
            return x.kind == k && x.event == event && (member < 0 || x.member == static_cast<unsigned>(member));
        }, timeout_ms, &e)) {
        Die("%s: no %s event 0x%x%s within %d ms", g_me.c_str(), kind, event,
            member < 0 ? "" : " for that member", timeout_ms);
    }
    return e;
}

template <typename T>
T At(std::int64_t base, std::size_t offset) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(static_cast<std::intptr_t>(base)) + offset,
                sizeof(T));
    return v;
}

// SceNpMatching2RoomMemberDataInternal as the game reads it: next +0, npId +0x10, memberId
// +0x38, flagAttr +0x3c.
Member MemberAt(std::int64_t m) {
    Member out;
    char name[17] = {};
    std::memcpy(name, reinterpret_cast<const char*>(static_cast<std::intptr_t>(m)) + 0x10, 16);
    out.name = name;
    out.id = At<std::uint16_t>(m, 0x38);
    out.owner = (At<std::uint32_t>(m, 0x3c) & 0x80000000u) != 0;
    return out;
}

// ---- the game's callbacks ----------------------------------------------------------------------

std::int64_t PS4_SYSV_ABI OnContext(std::int64_t, std::int64_t event, std::int64_t,
                                    std::int64_t error, std::int64_t, std::int64_t) {
    Event e;
    e.kind = "context";
    e.event = static_cast<int>(event);
    e.error = static_cast<int>(error);
    Post(std::move(e));
    return 0;
}

std::int64_t PS4_SYSV_ABI OnRequest(std::int64_t, std::int64_t, std::int64_t event,
                                    std::int64_t error, std::int64_t data, std::int64_t) {
    Event e;
    e.kind = "request";
    e.event = static_cast<int>(event);
    e.error = static_cast<int>(error);
    if (data && (event == 0x101 || event == 0x102)) {
        // CreateJoinRoomResponse: roomData +0, the member list (members, membersNum, me,
        // owner) from +8; RoomDataInternal: maxSlot +8, roomId +0x18.
        const std::int64_t room = At<std::int64_t>(data, 0);
        e.room = At<std::uint64_t>(room, 0x18);
        e.max_slot = At<std::uint16_t>(room, 8);
        const std::uint64_t n = At<std::uint64_t>(data, 0x10);
        std::int64_t m = At<std::int64_t>(data, 8);
        for (std::uint64_t i = 0; i < n && m; ++i, m = At<std::int64_t>(m, 0)) {
            e.members.push_back(MemberAt(m));
        }
        if (const std::int64_t me = At<std::int64_t>(data, 0x18)) e.me = MemberAt(me).id;
        if (const std::int64_t owner = At<std::int64_t>(data, 0x20)) e.owner = MemberAt(owner).id;
    }
    Post(std::move(e));
    return 0;
}

std::int64_t PS4_SYSV_ABI OnRoomEvent(std::int64_t, std::int64_t room, std::int64_t event,
                                      std::int64_t data, std::int64_t, std::int64_t) {
    Event e;
    e.kind = "room";
    e.event = static_cast<int>(event);
    e.room = static_cast<std::uint64_t>(room);
    if (data && (event == 0x1101 || event == 0x1102)) {
        // RoomMemberUpdateInfo: the member +0, eventCause +8.
        if (const std::int64_t m = At<std::int64_t>(data, 0)) {
            e.members.push_back(MemberAt(m));
            e.member = e.members.back().id;
        }
        e.cause = At<std::uint8_t>(data, 8);
    } else if (data && (event == 0x1103 || event == 0x1104)) {
        // RoomUpdateInfo: eventCause +0, optData +8, optDataLen +0x18.
        e.cause = At<std::uint8_t>(data, 0);
        const std::uint64_t n = At<std::uint64_t>(data, 0x18);
        for (std::uint64_t i = 0; i < n && i < 16; ++i) e.opt.push_back(At<std::uint8_t>(data, 8 + i));
    }
    Post(std::move(e));
    return 0;
}

std::int64_t PS4_SYSV_ABI OnSignaling(std::int64_t, std::int64_t room, std::int64_t member,
                                      std::int64_t event, std::int64_t error, std::int64_t) {
    Event e;
    e.kind = "signaling";
    e.event = static_cast<int>(event);
    e.error = static_cast<int>(error);
    e.room = static_cast<std::uint64_t>(room);
    e.member = static_cast<unsigned>(member);
    Post(std::move(e));
    return 0;
}

std::int64_t PS4_SYSV_ABI OnNpSignaling(std::int64_t, std::int64_t conn, std::int64_t event,
                                        std::int64_t error, std::int64_t, std::int64_t) {
    Event e;
    e.kind = "npsignaling";
    e.event = static_cast<int>(event);
    e.error = static_cast<int>(error);
    e.member = static_cast<unsigned>(conn);
    Post(std::move(e));
    return 0;
}

// ---- the game's P2P socket (vport 30, the session's) -------------------------------------------

constexpr std::uint16_t kVport = 30;

void Send(std::uint32_t addr, std::uint16_t port_nbo, const std::string& text,
          std::uint16_t dst_vport = kVport) {
    const int sent = Hd::P2pSend(kVport, dst_vport, text.data(),
                                 static_cast<std::uint32_t>(text.size()), addr, port_nbo);
    if (g_trace) std::printf("%6lld ms %s tx [%s] -> %d\n", Ms(), g_me.c_str(), text.c_str(), sent);
}

void OnDatagram(const Hd::P2pDatagram& dg) {
    Event e;
    e.kind = "datagram";
    e.text.assign(reinterpret_cast<const char*>(dg.data), dg.size);
    e.addr = dg.addr;
    e.port = dg.port;
    e.vport = dg.src_vport;
    if (g_trace) std::printf("%6lld ms %s rx [%s]\n", Ms(), g_me.c_str(), e.text.c_str());
    if (!dg.has_header || dg.dst_vport != kVport) {
        Die("%s: a datagram without the P2P header, or for vport %u", g_me.c_str(), dg.dst_vport);
    }
    if (e.text.rfind("PING ", 0) == 0) {
        // The game answers the address the datagram came from.
        Send(dg.addr, dg.port, "PONG " + g_me + e.text.substr(e.text.rfind(' ')), dg.src_vport);
        g_pings_answered.fetch_add(1);
    }
    Post(std::move(e));
}

// ---- the imports, by name, as the loader binds them ---------------------------------------------

using AnyFn = std::int64_t (PS4_SYSV_ABI*)(std::int64_t, std::int64_t, std::int64_t, std::int64_t,
                                           std::int64_t, std::int64_t);
int Call(const char* name, std::int64_t a0 = 0, std::int64_t a1 = 0, std::int64_t a2 = 0,
         std::int64_t a3 = 0, std::int64_t a4 = 0, std::int64_t a5 = 0) {
    const std::uintptr_t fn = Hd::Resolve(name);
    if (!fn) Die("the backend has no %s", name);
    return static_cast<int>(reinterpret_cast<AnyFn>(fn)(a0, a1, a2, a3, a4, a5));
}
std::int64_t P(const void* p) {
    return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(p));
}
std::int64_t F(AnyFn fn) {
    return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(fn));
}

struct RequestOptParam {
    void* cbFunc;
    void* cbFuncArg;
    std::uint32_t timeout;
    std::uint16_t appReqId;
    std::uint16_t pad;
};
struct KickoutRequest {
    std::uint64_t roomId;
    std::uint16_t memberId;
    std::uint8_t blockKickFlag;
    std::uint8_t pad[5];
    std::uint8_t optData[16];
    std::uint64_t optDataLen;
};

void NpId(char out[36], const std::string& name) {
    std::memset(out, 0, 36);
    std::snprintf(out, 17, "%s", name.c_str());
}

// Datagrams both ways: ours answered by the peer, and at least one of the peer's answered.
void Traffic(const std::string& peer, std::uint32_t addr, std::uint16_t port_nbo) {
    bool ponged = false;
    const std::string want = "PONG " + peer + " ";
    // One PING every 250 ms for up to 15 s: the peer starts its own a little later or earlier.
    for (int n = 1; n <= 60 && !(ponged && g_pings_answered.load() > 0); ++n) {
        const auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        Send(addr, port_nbo, "PING " + g_me + " " + std::to_string(n));
        if (Take([&](const Event& e) { return e.kind == "datagram" && e.text.rfind(want, 0) == 0; }, 250,
                 nullptr)) {
            ponged = true;
        }
        std::this_thread::sleep_until(next);
    }
    if (!ponged) Die("%s: no PONG from %s", g_me.c_str(), peer.c_str());
    if (g_pings_answered.load() == 0) Die("%s: no PING from %s arrived", g_me.c_str(), peer.c_str());
    std::printf("%s: datagrams both ways with %s\n", g_me.c_str(), peer.c_str());
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) Die("usage: hd-session-test host|guest <peer online id> [--kick] [--hold-ms N]");
    const bool host = std::strcmp(argv[1], "host") == 0;
    const std::string peer = argv[2];
    bool kick = false;
    int hold_ms = 0;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--kick")) kick = true;
        if (!std::strcmp(argv[i], "--hold-ms") && i + 1 < argc) hold_ms = std::atoi(argv[++i]);
    }
    if (!Hd::Enabled()) Die("BB_ONLINE=1 and BB_ONLINE_BACKEND=hd are not set");

    // Who the game is told it is.
    char npid[36];
    if (Call("sceNpGetNpId", 1, P(npid)) != 0) Die("sceNpGetNpId failed");
    g_me = npid;
    int state = 0;
    Call("sceNpGetState", 1, P(&state));
    if (state != 2) Die("%s: not signed in (state %d)", g_me.c_str(), state);

    // The game binds its P2P socket at boot.
    if (!Hd::P2pOpen()) Die("%s: no P2P port", g_me.c_str());
    Hd::P2pSetDeliver(OnDatagram);

    Call("sceNpMatching2Initialize", 0);
    struct {
        const void* npid;
        std::uint64_t rest[4];
    } create_param = {npid, {}};
    unsigned ctx = 0, sigctx = 0, req = 0;
    if (Call("sceNpMatching2CreateContext", P(&create_param), P(&ctx)) != 0 || !ctx) Die("no context");
    Call("sceNpMatching2RegisterContextCallback", F(OnContext), 0);
    Call("sceNpMatching2RegisterRoomEventCallback", ctx, F(OnRoomEvent), 0);
    Call("sceNpMatching2RegisterSignalingCallback", ctx, F(OnSignaling), 0);
    RequestOptParam dflt{reinterpret_cast<void*>(OnRequest), nullptr, 0, 0, 0};
    Call("sceNpMatching2SetDefaultRequestOptParam", ctx, P(&dflt));
    Call("sceNpSignalingInitialize", 0, 0, 0, 0);
    if (Call("sceNpSignalingCreateContext", P(npid), F(OnNpSignaling), 0, P(&sigctx)) != 0) {
        Die("no signaling context");
    }
    if (Call("sceNpMatching2ContextStart", ctx, 0) != 0) Die("ContextStart failed");
    Expect("context", 0x6F02);
    std::printf("%s: context started\n", g_me.c_str());
    if (!std::strcmp(argv[1], "signin")) {
        // Only the sign-in: the context's start and the event poll for --hold-ms; no room.
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        std::printf("RESULT ok\n");
        std::fflush(stdout);
        std::_Exit(0);
    }

    int status = 0;
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    std::uint64_t room = 0;
    const auto room_made = std::chrono::steady_clock::now();
    if (host) {
        std::uint8_t create[64] = {};
        const std::uint16_t max_slot = 5;
        std::memcpy(create, &max_slot, 2);
        if (Call("sceNpMatching2CreateJoinRoom", ctx, P(create), 0, P(&req)) != 0) Die("CreateJoinRoom");
        const Event made = Expect("request", 0x101);
        if (made.error || !made.room || made.me != 1 || made.owner != 1 || made.max_slot != 5 ||
            made.members.size() != 1 || made.members[0].name != g_me || !made.members[0].owner) {
            Die("host: a wrong room answer (error 0x%x room %llu me %u owner %u slots %u)",
                static_cast<unsigned>(made.error), static_cast<unsigned long long>(made.room), made.me,
                made.owner, made.max_slot);
        }
        room = made.room;
        std::printf("ROOM %llu\n", static_cast<unsigned long long>(room));
        Expect("signaling", 0x5102, 15000, 1);

        // The summon: a connection to the guest before it is a member of the room.
        char peer_npid[36];
        NpId(peer_npid, peer);
        unsigned conn = 0;
        if (Call("sceNpSignalingActivateConnection", sigctx, P(peer_npid), P(&conn)) != 0 || !conn) {
            Die("host: ActivateConnection");
        }
        Expect("npsignaling", 0x01, 15000, static_cast<int>(conn));
        Expect("npsignaling", 0x0c, 15000, static_cast<int>(conn));
        Call("sceNpSignalingGetConnectionStatus", sigctx, conn, P(&status), P(&addr), P(&port));
        if (status != 2 || !addr || !port) Die("host: the guest was not resolved (status %d)", status);

        // The invite, over the P2P port, until the guest has joined.
        Event joined;
        bool have = false;
        for (int i = 0; i < 80 && !have; ++i) {
            Send(addr, port, "INVITE " + g_me + " " + std::to_string(room));
            have = Take([](const Event& e) { return e.kind == "room" && e.event == 0x1101; }, 300, &joined);
        }
        if (!have) Die("host: the guest never joined");
        if (joined.room != room || joined.members.empty() || joined.members[0].name != peer ||
            joined.member != 2) {
            Die("host: a wrong MEMBER_JOINED (member %u)", joined.member);
        }
        Expect("signaling", 0x5102, 15000, 2);
        status = 0;
        Call("sceNpMatching2SignalingGetConnectionStatus", ctx, static_cast<std::int64_t>(room), 2,
             P(&status), P(&addr), P(&port));
        if (status != 2 || !addr || !port) Die("host: member 2 has no address (status %d)", status);
        std::printf("host: %s joined as member 2\n", peer.c_str());
        Traffic(peer, addr, port);

        if (kick) {
            KickoutRequest k{};
            k.roomId = room;
            k.memberId = 2;
            const std::uint8_t reason[4] = {0x19, 0x00, 0x00, 0xff};
            std::memcpy(k.optData, reason, 4);
            k.optDataLen = 4;
            if (Call("sceNpMatching2KickoutRoomMember", ctx, P(&k), 0, P(&req)) != 0) Die("Kickout");
            Expect("request", 0x104);
        }
        // The guest goes: the signaling DEAD for it, then MEMBER_LEFT with the cause.
        Expect("signaling", 0x5101, 20000, 2);
        const Event left = Expect("room", 0x1102, 20000);
        if (left.member != 2 || left.cause != (kick ? 2u : 1u)) {
            Die("host: a wrong MEMBER_LEFT (member %u cause %u)", left.member, left.cause);
        }
        std::printf("host: member 2 left (cause %u)\n", left.cause);
        // Stay in the room long enough for a heartbeat to be due.
        std::this_thread::sleep_until(room_made + std::chrono::milliseconds(hold_ms));
        status = 7;
        Call("sceNpMatching2SignalingGetConnectionStatus", ctx, static_cast<std::int64_t>(room), 2,
             P(&status), P(&addr), P(&port));
        if (status != 0) Die("host: a member that left still has a connection (status %d)", status);
    } else {
        // The invite arrives as a datagram from the host: the room to join.
        Event invite;
        if (!Take([](const Event& e) { return e.kind == "datagram" && e.text.rfind("INVITE ", 0) == 0; },
                  30000, &invite)) {
            Die("guest: no invite datagram");
        }
        room = std::strtoull(invite.text.substr(invite.text.rfind(' ') + 1).c_str(), nullptr, 10);
        std::printf("guest: invited into room %llu\n", static_cast<unsigned long long>(room));
        std::uint8_t join[32] = {};
        std::memcpy(join, &room, 8);
        if (Call("sceNpMatching2JoinRoom", ctx, P(join), 0, P(&req)) != 0) Die("JoinRoom");
        const Event in = Expect("request", 0x102);
        bool host_listed = false;
        for (const Member& m : in.members) host_listed |= m.id == 1 && m.name == peer && m.owner;
        if (in.error || in.room != room || in.me != 2 || in.owner != 1 || in.max_slot != 5 ||
            in.members.size() != 2 || !host_listed) {
            Die("guest: a wrong join answer (error 0x%x me %u owner %u slots %u members %zu)",
                static_cast<unsigned>(in.error), in.me, in.owner, in.max_slot, in.members.size());
        }
        Expect("signaling", 0x5102, 15000, 1);
        Call("sceNpMatching2SignalingGetConnectionStatus", ctx, static_cast<std::int64_t>(room), 1,
             P(&status), P(&addr), P(&port));
        if (status != 2 || !addr || !port) Die("guest: the host has no address (status %d)", status);
        std::printf("guest: joined as member 2, host is member 1\n");
        Traffic(peer, addr, port);

        if (kick) {
            // KICKEDOUT, with the 4-byte reason the host passed, after a DEAD for the host.
            const Event out = Expect("room", 0x1103, 20000);
            const std::vector<std::uint8_t> reason = {0x19, 0x00, 0x00, 0xff};
            if (out.cause != 2 || out.opt != reason) Die("guest: a wrong KICKEDOUT (cause %u)", out.cause);
            Expect("signaling", 0x5101, 15000, 1);
            std::printf("guest: kicked, with the host's reason\n");
        } else {
            // Give the host's last datagrams their answers, then leave.
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }
    }

    std::uint8_t leave[32] = {};
    std::memcpy(leave, &room, 8);
    if (Call("sceNpMatching2LeaveRoom", ctx, P(leave), 0, P(&req)) != 0) Die("LeaveRoom");
    Expect("request", 0x103);
    std::printf("RESULT ok\n");
    std::fflush(stdout);
    std::_Exit(0);
}
