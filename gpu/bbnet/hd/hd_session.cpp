// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/net/session.cpp.
#include "hd/hd_session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

#include "common/types.h"
#include "hd/hd.h"
#include "hd/hd_account.h"
#include "hd/hd_p2p.h"

extern "C" {
// src/runtime_thread.c: a guest thread of the runtime (it may call the game's callbacks).
std::int32_t runtime_thread_spawn(void** out, void* PS4_SYSV_ABI (*entry)(void*), void* argument,
                                  std::uint64_t stack, const char* name);
}

namespace Hd {

namespace {

using Clock = std::chrono::steady_clock;

// ---- the dispatcher -------------------------------------------------------

struct Due {
    Clock::time_point at;
    int prio;
    std::uint64_t seq;
    std::function<void()> fn;
    bool operator<(const Due& o) const {
        if (at != o.at) return at < o.at;
        if (prio != o.prio) return prio < o.prio;
        return seq < o.seq;
    }
};

std::mutex g_dmu;
std::condition_variable g_dcv;
std::deque<Due> g_due; // kept sorted on insert
std::uint64_t g_seq = 0;
std::atomic<bool> g_drun{false};
std::atomic<std::thread::id> g_dtid{};

// The thread also runs the requests to the server the callbacks wait on (cpp-httplib and
// its TLS), so it has a host-sized stack.
constexpr std::uint64_t kDispatcherStack = 1024 * 1024;

void* PS4_SYSV_ABI DispatcherMain(void*) {
    g_dtid.store(std::this_thread::get_id());
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(g_dmu);
            for (;;) {
                if (!g_drun.load()) break;
                if (!g_due.empty()) {
                    const auto now = Clock::now();
                    if (g_due.front().at <= now) {
                        fn = std::move(g_due.front().fn);
                        g_due.pop_front();
                        break;
                    }
                    const auto until = g_due.front().at;
                    g_dcv.wait_until(lk, until);
                } else {
                    g_dcv.wait(lk);
                }
            }
            if (!g_drun.load() && !fn) break;
        }
        if (fn) {
            try {
                fn();
            } catch (const std::exception& e) {
                Log("np: a scheduled call failed: %s", e.what());
            }
        }
    }
    return nullptr;
}

void DispatcherEnsureLocked() {
    if (g_drun.load()) return;
    g_drun.store(true);
    void* thread = nullptr;
    const std::int32_t rc =
        runtime_thread_spawn(&thread, DispatcherMain, nullptr, kDispatcherStack, "bb-np");
    if (rc != 0) {
        g_drun.store(false);
        Log("np: the dispatcher thread could not be started (%d); online calls will not answer",
            rc);
    }
}

// ---- the peer table -------------------------------------------------------

std::mutex g_pmu;
std::map<std::uint16_t, Peer> g_peers;

// "a.b.c.d" as a network-order address, 0 for anything else.
std::uint32_t AddrOf(const std::string& text) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    char extra = 0;
    if (text.empty() || std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) {
        return 0;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    const std::uint8_t bytes[4] = {static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b),
                                   static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(d)};
    std::uint32_t v = 0;
    std::memcpy(&v, bytes, 4);
    return v;
}

std::string AddrText(std::uint32_t addr_nbo) {
    std::uint8_t b[4];
    std::memcpy(b, &addr_nbo, 4);
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

// ---- the poller -----------------------------------------------------------

std::mutex g_pollmu;
std::atomic<bool> g_pollrun{false};
std::condition_variable g_pollcv;
std::function<void(const Json&)> g_handler;
long long g_cursor = 0;

// The event's fields sit beside Name in the reply; a server that nests them under Payload
// is read the same way.
Json FlatEvent(const Json& reply) {
    Json ev = reply;
    if (reply.is_object()) {
        const auto payload = reply.find("Payload");
        if (payload != reply.end() && payload->is_object()) {
            for (auto it = payload->begin(); it != payload->end(); ++it) {
                if (!ev.contains(it.key())) ev[it.key()] = it.value();
            }
        }
    }
    return ev;
}

void PollerMain() {
    const std::string me = OnlineId();
    int interval_ms = 250;
    while (g_pollrun.load()) {
        Json body = Json::object();
        body["OnlineId"] = me;
        body["Cursor"] = g_cursor;
        body["Categories"] = Json::array({"matching2"});
        Json reply;
        std::string err;
        if (NpPost("/np/events/poll", body, reply, err, 4000)) {
            interval_ms = 250;
            const long long next = IntOf(reply, "NextCursor", g_cursor);
            if (IntOf(reply, "HasEvent", 0) != 0) {
                std::function<void(const Json&)> h;
                {
                    std::lock_guard<std::mutex> lk(g_pollmu);
                    h = g_handler;
                }
                Log("np: event #%lld %s", IntOf(reply, "EventId", 0),
                    StrOf(reply, "Name").c_str());
                if (h) {
                    try {
                        h(FlatEvent(reply));
                    } catch (const std::exception& e) {
                        Log("np: the event could not be handled: %s", e.what());
                    }
                }
                Json ack = Json::object();
                ack["OnlineId"] = me;
                ack["Cursor"] = next;
                Json ackreply;
                std::string ackerr;
                NpPost("/np/events/ack", ack, ackreply, ackerr, 4000);
                g_cursor = next;
                continue; // drain without waiting when there was one
            }
            g_cursor = next > g_cursor ? next : g_cursor;
        } else if (AccountRefused()) {
            // The server refused the sign-in (hd_account.h): nothing will arrive until it
            // is linked again, so ask once a minute, not every 4 s.
            interval_ms = 60000;
        } else {
            // Back off while the server is unreachable; the game keeps running.
            interval_ms = interval_ms < 4000 ? interval_ms * 2 : 4000;
        }
        std::unique_lock<std::mutex> lk(g_pollmu);
        g_pollcv.wait_for(lk, std::chrono::milliseconds(interval_ms),
                          [] { return !g_pollrun.load(); });
    }
}

// The reflexive address of our P2P port: a STUN Binding exchange with BB_HD_STUN_SERVER
// (default: the private server's host on 3478, "off" disables), asked at context_start and
// again when the cached answer is over two minutes old. The server records it as
// MappedAddr/MappedPort and hands it to peers instead of the LAN address when the two
// differ.
struct Mapped {
    std::mutex mu;
    std::string addr;
    int port = 0;
    std::int64_t at_ms = 0;
    bool asked = false;
};
Mapped g_mapped;
std::atomic<int> g_stun_rtt_us{0};

void StunServer(std::string* host, std::uint16_t* port) {
    std::string spec = Cfg().stun_server;
    if (spec.empty()) {
        // The private server's host, from the API base (scheme://host[:port]).
        spec = HostOfUrl(NpServerBase());
    }
    *port = 3478;
    const std::size_t colon = spec.find(':');
    if (colon != std::string::npos) {
        *port = static_cast<std::uint16_t>(std::atoi(spec.c_str() + colon + 1));
        spec = spec.substr(0, colon);
    }
    *host = spec;
}

void MappedRefresh(bool force) {
    std::lock_guard<std::mutex> lk(g_mapped.mu);
    if (!force && g_mapped.asked && NowMs() - g_mapped.at_ms < 120000) return;
    g_mapped.asked = true;
    g_mapped.at_ms = NowMs();
    if (Cfg().stun_server == "off") return;
    std::string host;
    std::uint16_t sport = 0;
    StunServer(&host, &sport);
    if (host.empty()) return;
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    bool ok = false;
    // The relay HELLO unless BB_HD_RELAY=0 (then datagrams go to the server's relay ports
    // as addressed, the per-port relay of PS4 clients).
    const bool use_relay = Cfg().relay;
    Stun::Relay relay;
    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
        const auto t0 = Clock::now();
        ok = P2pStun(host.c_str(), sport, 1000, &addr, &port, use_relay ? &relay : nullptr);
        if (ok) {
            g_stun_rtt_us = static_cast<int>(
                std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        }
    }
    if (!ok) {
        if (!g_mapped.addr.empty()) return; // keep the last answer
        Log("stun: no answer from %s:%u; the server gets the LAN address only (BB_HD_P2P_ADDR)",
            host.c_str(), sport);
        return;
    }
    const std::string text = AddrText(addr);
    if (g_mapped.addr != text || g_mapped.port != port) {
        Log("stun: %s:%u sees our P2P port %d as %s:%u%s", host.c_str(), sport, P2pPortNumber(),
            text.c_str(), port,
            (text == LocalAddrText() && port == P2pPortNumber()) ? " (no NAT in between)" : "");
    }
    g_mapped.addr = text;
    g_mapped.port = port;
}

Json OurEndpoint() {
    MappedRefresh(false);
    Json o = Json::object();
    o["OnlineId"] = OnlineId();
    o["LocalAddr"] = LocalAddrText();
    o["LocalPort"] = P2pPortNumber();
    o["PublicAddr"] = LocalAddrText();
    o["PublicPort"] = P2pPortNumber();
    std::lock_guard<std::mutex> lk(g_mapped.mu);
    o["MappedAddr"] = g_mapped.addr;
    o["MappedPort"] = g_mapped.port;
    return o;
}

// A peer with a new address: probes toward it so our NAT lets its datagrams in
// (P2pPunch), never toward ourselves.
void PunchIfNew(const Peer& before, const Peer& after) {
    if (after.online_id == OnlineId() || !after.addr || !after.port) return;
    if (before.addr == after.addr && before.port == after.port &&
        before.local_addr == after.local_addr && before.local_port == after.local_port) {
        return;
    }
    P2pPunch(after.online_id.c_str(), after.addr, after.port, after.local_addr, after.local_port);
}

std::atomic<int> g_p2p_port_bound{0};

} // namespace

void DispatchAfter(int delay_ms, Prio prio, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_dmu);
    DispatcherEnsureLocked();
    Due d{Clock::now() + std::chrono::milliseconds(delay_ms < 0 ? 0 : delay_ms),
          static_cast<int>(prio), g_seq++, std::move(fn)};
    auto it = g_due.begin();
    while (it != g_due.end() && *it < d) ++it;
    g_due.insert(it, std::move(d));
    g_dcv.notify_all();
}

bool OnDispatcher() {
    return g_dtid.load() == std::this_thread::get_id();
}

void SessionShutdown() {
    PollerStop();
    std::lock_guard<std::mutex> lk(g_dmu);
    if (!g_drun.load()) return;
    g_drun.store(false);
    g_dcv.notify_all();
}

std::string OnlineId() {
    // The account's name when signed in; the server enforces the same through the token
    // header, this just keeps the two in step.
    if (const std::string acct = AccountName(); !acct.empty()) return acct;
    const std::string& id = Cfg().online_id;
    return id.empty() ? "Player" : id;
}

std::string LocalAddrText() {
    const std::string& a = Cfg().p2p_addr;
    return a.empty() ? "127.0.0.1" : a;
}

int P2pPortNumber() {
    if (const int b = g_p2p_port_bound.load(std::memory_order_relaxed); b > 0) return b;
    return Cfg().p2p_port;
}

void SetP2pPortBound(int port) {
    g_p2p_port_bound.store(port, std::memory_order_relaxed);
}

void PeersClear() {
    std::lock_guard<std::mutex> lk(g_pmu);
    g_peers.clear();
}

void PeersUpsert(const Peer& p) {
    Peer before;
    {
        std::lock_guard<std::mutex> lk(g_pmu);
        Peer& slot = g_peers[p.member_id];
        before = slot;
        const unsigned conn = p.conn_id ? p.conn_id : slot.conn_id;
        const int sig = p.sig_status ? p.sig_status : slot.sig_status;
        slot = p;
        slot.conn_id = conn;
        slot.sig_status = sig;
    }
    PunchIfNew(before, p);
}

bool PeersGet(std::uint16_t member_id, Peer* out) {
    std::lock_guard<std::mutex> lk(g_pmu);
    const auto it = g_peers.find(member_id);
    if (it == g_peers.end()) return false;
    if (out) *out = it->second;
    return true;
}

bool PeersFindOnline(const std::string& id, Peer* out) {
    std::lock_guard<std::mutex> lk(g_pmu);
    for (const auto& [m, p] : g_peers) {
        if (p.online_id == id) {
            if (out) *out = p;
            return true;
        }
    }
    return false;
}

bool PeersFindConn(unsigned conn_id, Peer* out) {
    std::lock_guard<std::mutex> lk(g_pmu);
    for (const auto& [m, p] : g_peers) {
        if (p.conn_id == conn_id && conn_id != 0) {
            if (out) *out = p;
            return true;
        }
    }
    return false;
}

void PeersSetConn(std::uint16_t member_id, unsigned conn_id, int sig_status) {
    std::lock_guard<std::mutex> lk(g_pmu);
    const auto it = g_peers.find(member_id);
    if (it == g_peers.end()) return;
    it->second.conn_id = conn_id;
    it->second.sig_status = sig_status;
}

void PeersErase(std::uint16_t member_id) {
    std::lock_guard<std::mutex> lk(g_pmu);
    g_peers.erase(member_id);
}

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
}

std::vector<Peer> PeersAll() {
    std::lock_guard<std::mutex> lk(g_pmu);
    std::vector<Peer> v;
    for (const auto& [m, p] : g_peers) v.push_back(p);
    return v;
}

int PeersFromMembers(const Json& members) {
    if (!members.is_array()) return 0;
    int n = 0;
    for (const Json& m : members) {
        Peer p;
        p.member_id = static_cast<std::uint16_t>(IntOf(m, "MemberId", 0));
        if (!p.member_id) continue;
        p.online_id = StrOf(m, "OnlineId");
        p.addr = AddrOf(StrOf(m, "Addr"));
        p.port = static_cast<std::uint16_t>(IntOf(m, "Port", 0));
        p.local_addr = AddrOf(StrOf(m, "LocalAddr"));
        p.local_port = static_cast<std::uint16_t>(IntOf(m, "LocalPort", 0));
        // The server's own rule, applied here too in case a record carries the raw public
        // address: a STUN-mapped address that differs from the peer's local one means a NAT
        // in between, and only the mapped one reaches the peer.
        const std::uint32_t mapped = AddrOf(StrOf(m, "MappedAddr"));
        const int mapped_port = static_cast<int>(IntOf(m, "MappedPort", 0));
        if (mapped && mapped_port > 0 && mapped_port <= 65535 && mapped != p.local_addr &&
            mapped != p.addr) {
            p.addr = mapped;
            p.port = static_cast<std::uint16_t>(mapped_port);
        }
        if (!p.addr) {
            p.addr = p.local_addr;
            p.port = p.local_port;
        }
        PeersUpsert(p);
        ++n;
    }
    return n;
}

bool ServerSignalingResolve(const std::string& id, std::uint32_t* addr, std::uint16_t* port,
                            std::string& error) {
    Json body = Json::object();
    body["OnlineId"] = id;
    Json reply;
    if (!NpPost("/np/signaling/resolve", body, reply, error)) return false;
    if (IntOf(reply, "ResKind", -1) != 0) {
        error = "not registered";
        return false;
    }
    const std::uint32_t a = AddrOf(StrOf(reply, "Addr"));
    const int p = static_cast<int>(IntOf(reply, "Port", 0));
    if (!a || p <= 0 || p > 65535) {
        error = "no address";
        return false;
    }
    if (addr) *addr = a;
    if (port) *port = static_cast<std::uint16_t>(p);
    return true;
}

Peer PeersProvisional(const std::string& id, std::uint32_t addr, std::uint16_t port) {
    Peer before, after;
    {
        std::lock_guard<std::mutex> lk(g_pmu);
        bool found = false;
        for (auto& [m, p] : g_peers) {
            if (p.online_id == id && m >= 0xff00) {
                before = p;
                p.addr = addr;
                p.port = port;
                after = p;
                found = true;
                break;
            }
        }
        if (!found) {
            std::uint16_t m = 0xff00;
            while (g_peers.count(m) && m != 0xffff) ++m;
            after.member_id = m;
            after.online_id = id;
            after.addr = addr;
            after.port = port;
            g_peers[m] = after;
        }
    }
    PunchIfNew(before, after);
    return after;
}

bool ServerContextStart(Json& reply, std::string& error) {
    MappedRefresh(true);
    Json body = Json::object();
    body["OnlineId"] = OnlineId();
    body["SignalingAddr"] = LocalAddrText();
    body["SignalingPort"] = P2pPortNumber();
    {
        std::lock_guard<std::mutex> lk(g_mapped.mu);
        body["MappedAddr"] = g_mapped.addr;
        body["MappedPort"] = g_mapped.port;
    }
    return NpPost("/mp/matching2/context_start", body, reply, error);
}

bool ServerCreateRoom(int max_members, const Json& extra, Json& reply, std::string& error) {
    Json body = OurEndpoint();
    body["MaxMembers"] = max_members;
    if (extra.is_object()) {
        for (auto it = extra.begin(); it != extra.end(); ++it) body[it.key()] = it.value();
    }
    return NpPost("/mp/matching2/create_room", body, reply, error);
}

bool ServerJoinRoom(std::uint64_t room_id, Json& reply, std::string& error) {
    Json body = OurEndpoint();
    body["RoomId"] = static_cast<long long>(room_id);
    return NpPost("/mp/matching2/join_room", body, reply, error);
}

bool ServerLeaveRoom(const std::string& session_id, int member_id, Json& reply,
                     std::string& error) {
    Json body = Json::object();
    body["SessionId"] = session_id;
    body["MemberId"] = member_id;
    return NpPost("/mp/matching2/leave_room", body, reply, error);
}

int ServerHeartbeat(const std::string& session_id, int member_id) {
    Json body = Json::object();
    body["SessionId"] = session_id;
    body["MemberId"] = member_id;
    Json reply;
    std::string err;
    if (!NpPost("/mp/matching2/heartbeat", body, reply, err, 3000)) return -1;
    // A server from before 2026-10-06 answers OK whatever happened.
    return IntOf(reply, "InRoom", 1) == 0 ? 0 : 1;
}

bool ServerKickMember(const std::string& session_id, int member_id, int kicker_id,
                      const std::uint8_t* opt, std::size_t opt_len, std::string& error) {
    static const char* const b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string enc;
    opt_len = opt_len > 16 ? 16 : opt_len;
    for (std::size_t i = 0; i < opt_len; i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(opt[i]) << 16) |
                                (i + 1 < opt_len ? static_cast<std::uint32_t>(opt[i + 1]) << 8 : 0u) |
                                (i + 2 < opt_len ? opt[i + 2] : 0u);
        enc += b64[(v >> 18) & 63];
        enc += b64[(v >> 12) & 63];
        enc += i + 1 < opt_len ? b64[(v >> 6) & 63] : '=';
        enc += i + 2 < opt_len ? b64[v & 63] : '=';
    }
    Json body = Json::object();
    body["SessionId"] = session_id;
    body["MemberId"] = member_id;
    body["KickerMemberId"] = kicker_id;
    body["OptData"] = enc;
    Json reply;
    return NpPost("/mp/matching2/kick_member", body, reply, error, 4000);
}

bool ServerSessionBlob(Json& reply, std::string& error) {
    return NpGet("/mp/matching2/session_blob?OnlineId=" + OnlineId(), reply, error);
}

namespace {

std::mutex g_roommu;
std::string g_room_sid;
int g_room_mid = 0;

// Tells the server our new reflexive address (a relay port that changed: the server
// restarted, the port was released while we were away).
void ServerSignalingUpdate() {
    Json body = Json::object();
    body["OnlineId"] = OnlineId();
    {
        std::lock_guard<std::mutex> lk(g_mapped.mu);
        body["MappedAddr"] = g_mapped.addr;
        body["MappedPort"] = g_mapped.port;
    }
    {
        std::lock_guard<std::mutex> lk(g_roommu);
        body["SessionId"] = g_room_sid;
        body["MemberId"] = g_room_mid;
    }
    Json reply;
    std::string err;
    NpPost("/mp/matching2/signaling_update", body, reply, err, 4000);
}

// While the context runs: the STUN binding every 15 s. It keeps our NAT mapping to the
// server open (a router forgets an idle UDP mapping in 30 s to a few minutes, and then
// nothing the relay sends gets in) and the relay port alive (released after 10 idle
// minutes). A changed address goes to the server at once.
std::atomic<bool> g_keeprun{false};
std::condition_variable g_keepcv;
std::mutex g_keepmu;

void KeepaliveMain() {
    std::string last;
    {
        std::lock_guard<std::mutex> lk(g_mapped.mu);
        last = g_mapped.addr + ":" + std::to_string(g_mapped.port);
    }
    while (g_keeprun.load()) {
        {
            std::unique_lock<std::mutex> lk(g_keepmu);
            g_keepcv.wait_for(lk, std::chrono::seconds(15), [] { return !g_keeprun.load(); });
        }
        if (!g_keeprun.load()) break;
        MappedRefresh(true);
        std::string now;
        {
            std::lock_guard<std::mutex> lk(g_mapped.mu);
            now = g_mapped.addr + ":" + std::to_string(g_mapped.port);
        }
        if (now != last && now != ":0") {
            Log("stun: our address is now %s (was %s); telling the server", now.c_str(),
                last.c_str());
            ServerSignalingUpdate();
            last = now;
        }
    }
}

} // namespace

void RoomSet(const std::string& session_id, int member_id) {
    std::lock_guard<std::mutex> lk(g_roommu);
    g_room_sid = session_id;
    g_room_mid = member_id;
}

void RoomClear() {
    RoomSet("", 0);
}

int StunRttUs() {
    return g_stun_rtt_us.load();
}

void PollerStart(std::function<void(const Json& event)> handler) {
    std::lock_guard<std::mutex> lk(g_pollmu);
    g_handler = std::move(handler);
    if (g_pollrun.load()) return;
    g_pollrun.store(true);
    g_cursor = 0;
    std::thread(PollerMain).detach();
    if (!g_keeprun.exchange(true)) {
        std::thread(KeepaliveMain).detach();
    }
}

void PollerStop() {
    {
        std::lock_guard<std::mutex> lk(g_keepmu);
        g_keeprun.store(false);
        g_keepcv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(g_pollmu);
        if (!g_pollrun.load()) return;
        g_pollrun.store(false);
        g_pollcv.notify_all();
    }
}

} // namespace Hd
