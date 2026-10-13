// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: the P2P socket of src/hle/net.cpp (the port, its reader, the header,
// the relay frames, the STUN exchange and the hole punch).
#include "hd/hd_p2p.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hd/hd.h"
#include "hd/hd_session.h"

namespace Hd {

namespace {

// The socket API behind a few names, so the layer reads the same on Linux and Windows
// (winsock: SOCKET handles, closesocket, WSA error numbers, no MSG_DONTWAIT - the socket is
// non-blocking and a wait is a poll).
#if defined(_WIN32)
using SockFd = SOCKET;
constexpr SockFd kBadSock = INVALID_SOCKET;
using SockLen = int;
inline void SockClose(SockFd fd) {
    ::closesocket(fd);
}
inline bool SockWouldBlock() {
    return ::WSAGetLastError() == WSAEWOULDBLOCK;
}
inline bool SockAddrInUse() {
    const int e = ::WSAGetLastError();
    return e == WSAEADDRINUSE || e == WSAEACCES; // a port another program holds exclusively
}
inline int SockError() {
    return ::WSAGetLastError();
}
inline void SockSetNonblock(SockFd fd) {
    u_long v = 1;
    ::ioctlsocket(fd, FIONBIO, &v);
}
inline int SockPoll(SockFd fd, short events, int ms) {
    WSAPOLLFD p{fd, events, 0};
    return ::WSAPoll(&p, 1, ms);
}
#else
using SockFd = int;
constexpr SockFd kBadSock = -1;
using SockLen = socklen_t;
inline void SockClose(SockFd fd) {
    ::close(fd);
}
inline bool SockWouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
inline bool SockAddrInUse() {
    return errno == EADDRINUSE || errno == EACCES;
}
inline int SockError() {
    return errno;
}
inline void SockSetNonblock(SockFd fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
inline int SockPoll(SockFd fd, short events, int ms) {
    pollfd p{fd, events, 0};
    return ::poll(&p, 1, ms);
}
#endif
inline bool SockOk(SockFd fd) {
    return fd != kBadSock;
}

std::uint16_t Bswap16(std::uint16_t v) {
    return static_cast<std::uint16_t>((v << 8) | (v >> 8));
}

void WriteIpv4(char* dst, std::size_t n, std::uint32_t net) {
    std::uint8_t b[4];
    std::memcpy(b, &net, 4);
    std::snprintf(dst, n, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

// The PS4 kernel multiplexes virtual ports over one UDP port with a header on every
// datagram: [0xff][flags][src vport][dst vport] when flag 0x40 says the vports are one byte
// (Bloodborne's are, 40 and 30), two-byte vports without it, four more bytes when flag 0x20
// adds a comid. bbhost keeps the header on the wire, and so does this.
constexpr std::uint8_t kP2pMagic = 0xff;
constexpr std::uint8_t kP2pFlagP2p = 0x80, kP2pFlagByteVports = 0x40, kP2pFlagComid = 0x20,
                       kP2pType = 0x03;
constexpr std::size_t kMaxHeader = 6;
// A hole-punch probe: not a P2P datagram (no 0xff), dropped by the reader.
constexpr std::uint8_t kProbe[8] = {0xfe, 'b', 'b', 'h', 'p', 0, 0, 0};

// The private server's relay (hd_stun.h): once a STUN answer carried BBHOST-RELAY, a
// datagram for another relay port (the server's address, not its STUN port) leaves framed
// for the STUN port - [0xfb]['R'][token][port] - and the relay's deliveries come back from
// the STUN port framed [0xfb]['r'][source port]. One destination keeps one NAT mapping, the
// one the keepalive (the same binding) holds open, so every NAT lets them in. The game sees
// none of it: its peers stay server:port both ways.
constexpr std::uint8_t kRelayMagic = 0xfb, kRelayToServer = 'R', kRelayToClient = 'r';
constexpr std::size_t kRelayHeader = 12;
constexpr std::size_t kMaxUdpPayload = 65507;

struct RelayLink {
    std::mutex mu;
    bool on = false;
    std::uint32_t server = 0;    // network order
    std::uint16_t stun_port = 0; // network order
    std::uint8_t token[Stun::kTokenLen] = {};
    std::uint16_t vport = 0;     // host order
};
RelayLink g_relay;

// One UDP socket for the instance, shared by every game socket that binds a vport on it
// (the game binds 40 at boot and 30 for a session). A reader thread drains it.
struct Port {
    SockFd fd = kBadSock;
    std::uint16_t port = 0; // host order
    std::mutex mu;
    std::atomic<bool> stop{false};
    std::uint64_t rx = 0, no_header = 0, probes = 0, tx = 0;
    std::chrono::steady_clock::time_point last_report{};
    // One STUN Binding exchange at a time (P2pStun): the reader answers the transaction it
    // finds here (under mu).
    bool stun_pending = false, stun_done = false;
    std::uint8_t stun_txid[Stun::kTxid] = {};
    std::uint32_t stun_addr = 0;
    std::uint16_t stun_port = 0;
    Stun::Relay stun_relay;
    std::condition_variable stun_cv;
    // When a datagram last arrived from each source (addr | port << 32, both network
    // order): the hole-punch stops once the peer is heard.
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> heard;
};

std::mutex g_mu;
std::shared_ptr<Port> g_port;
std::atomic<P2pDeliver> g_deliver{nullptr};
std::atomic<int> g_send_logs{0};

// The first datagrams are always logged (the P2P handshake is a few dozen small packets and
// the runs that fail are the ones without a trace); BB_HD_TRACE=1 removes the cap.
constexpr int kTraceFirst = 400;

// True when addr:port (network order) is a relay port to frame for.
bool RelayTarget(std::uint32_t addr, std::uint16_t port_nbo, std::uint8_t token[Stun::kTokenLen],
                 sockaddr_in* stun_sa) {
    std::lock_guard<std::mutex> lk(g_relay.mu);
    if (!g_relay.on || addr != g_relay.server || port_nbo == g_relay.stun_port) return false;
    std::memcpy(token, g_relay.token, Stun::kTokenLen);
    *stun_sa = sockaddr_in{};
    stun_sa->sin_family = AF_INET;
    stun_sa->sin_addr.s_addr = g_relay.server;
    stun_sa->sin_port = g_relay.stun_port;
    return true;
}

bool FromRelayServer(const sockaddr_in& sa) {
    std::lock_guard<std::mutex> lk(g_relay.mu);
    return g_relay.on && sa.sin_addr.s_addr == g_relay.server && sa.sin_port == g_relay.stun_port;
}

inline std::uint64_t SourceKey(std::uint32_t addr, std::uint16_t port_nbo) {
    return addr | (static_cast<std::uint64_t>(port_nbo) << 32);
}

// Splits a datagram at the P2P header; returns the header length (0 when there is none)
// and the vports.
std::size_t P2pHeader(const std::uint8_t* d, std::size_t n, std::uint16_t* src,
                      std::uint16_t* dst) {
    if (n < 4 || d[0] != kP2pMagic) return 0;
    const std::uint8_t fl = d[1];
    std::size_t at = 2;
    if (fl & kP2pFlagComid) at += 4;
    std::size_t hdr;
    if (fl & kP2pFlagByteVports) {
        if (at + 2 > n) return 0;
        *src = d[at];
        *dst = d[at + 1];
        hdr = at + 2;
    } else {
        if (at + 4 > n) return 0;
        *src = static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
        *dst = static_cast<std::uint16_t>((d[at + 2] << 8) | d[at + 3]);
        hdr = at + 4;
    }
    return hdr;
}

int SendTo(SockFd fd, const std::uint8_t* data, std::size_t len, const sockaddr_in& sa) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto n = ::sendto(fd, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                                reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
        if (n >= 0) return static_cast<int>(n);
        if (!SockWouldBlock()) return -1;
        SockPoll(fd, POLLOUT, 100);
    }
    return -1;
}

void Reader(std::shared_ptr<Port> port) {
    std::vector<std::uint8_t> storage(65536);
    std::uint8_t* const buf = storage.data();
    int logs = 0;
    while (!port->stop.load(std::memory_order_relaxed)) {
        const int ready = SockPoll(port->fd, POLLIN, 200);
        if (ready < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (ready == 0) continue;
        sockaddr_in sa{};
        SockLen sl = sizeof(sa);
        auto n = ::recvfrom(port->fd, reinterpret_cast<char*>(buf), static_cast<int>(storage.size()),
                            0, reinterpret_cast<sockaddr*>(&sa), &sl);
        if (n < 0) continue;
        if (n >= 4 && buf[0] == kRelayMagic && buf[1] == kRelayToClient && FromRelayServer(sa)) {
            // A relay delivery: the datagram of the owner of the source port, which the
            // game knows as server:port.
            const std::uint16_t from_vport = static_cast<std::uint16_t>((buf[2] << 8) | buf[3]);
            std::memmove(buf, buf + 4, static_cast<std::size_t>(n - 4));
            n -= 4;
            sa.sin_port = Bswap16(from_vport);
        }
        std::uint16_t src = 0, dst = 0;
        const std::size_t hdr = P2pHeader(buf, static_cast<std::size_t>(n), &src, &dst);
        if (!hdr) {
            // Not the game's: a peer's hole-punch probe, or the STUN server's answer to
            // our Binding Request. Neither reaches a game socket.
            if (n >= 8 && std::memcmp(buf, kProbe, 5) == 0) {
                std::lock_guard<std::mutex> lk(port->mu);
                ++port->rx;
                ++port->probes;
                port->heard[SourceKey(sa.sin_addr.s_addr, sa.sin_port)] =
                    std::chrono::steady_clock::now();
                continue;
            }
            if (n >= 20 && buf[0] == 0x01 && buf[1] == 0x01) {
                std::lock_guard<std::mutex> lk(port->mu);
                std::uint32_t a = 0;
                std::uint16_t p = 0;
                if (port->stun_pending &&
                    Stun::ParseBindingResponse(buf, static_cast<std::size_t>(n), port->stun_txid,
                                               &a, &p, &port->stun_relay)) {
                    ++port->rx;
                    port->stun_addr = a;
                    port->stun_port = p;
                    port->stun_done = true;
                    port->stun_pending = false;
                    port->stun_cv.notify_all();
                    continue;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(port->mu);
            ++port->rx;
            port->heard[SourceKey(sa.sin_addr.s_addr, sa.sin_port)] =
                std::chrono::steady_clock::now();
            if (!hdr) ++port->no_header;
            // One line every 60 s: the stream's shape.
            const auto now = std::chrono::steady_clock::now();
            if (now - port->last_report > std::chrono::seconds(60)) {
                port->last_report = now;
                Log("net: p2p port %u: rx %llu, tx %llu, no header %llu, probes %llu", port->port,
                    static_cast<unsigned long long>(port->rx),
                    static_cast<unsigned long long>(port->tx),
                    static_cast<unsigned long long>(port->no_header),
                    static_cast<unsigned long long>(port->probes));
            }
        }
        if (Cfg().trace || ++logs <= kTraceFirst) {
            char ip[32];
            WriteIpv4(ip, sizeof(ip), sa.sin_addr.s_addr);
            Log("net: recv <- %s:%u vport %u->%u: %zu bytes%s", ip, Bswap16(sa.sin_port), src, dst,
                static_cast<std::size_t>(n) - hdr, hdr ? "" : " (no p2p header)");
        }
        const P2pDeliver deliver = g_deliver.load(std::memory_order_acquire);
        if (!deliver) continue;
        P2pDatagram dg;
        dg.data = buf + hdr;
        dg.size = static_cast<std::size_t>(n) - hdr;
        dg.addr = sa.sin_addr.s_addr;
        dg.port = sa.sin_port;
        dg.src_vport = src;
        dg.dst_vport = dst;
        dg.has_header = hdr != 0;
        deliver(dg);
    }
}

// The instance's P2P UDP port, opened on first use (the game's first P2P bind, or the
// session layer's STUN query before it).
std::shared_ptr<Port> PortOpen() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_port) return g_port;
#if defined(_WIN32)
    static const bool wsa_started = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)wsa_started;
#endif
    auto port = std::make_shared<Port>();
    // The port is this instance's alone. With SO_REUSEADDR a second instance on the machine
    // bound it too and the kernel handed it our peers' datagrams. Windows also lets a later
    // SO_REUSEADDR socket take a port unless the first is exclusive. When another program
    // has it, the next free port is ours and becomes what the server is told.
    const auto want = static_cast<std::uint16_t>(Cfg().p2p_port);
    constexpr int kTries = 16;
    for (int i = 0; i < kTries; ++i) {
        const auto at = static_cast<std::uint16_t>(want + i);
        port->fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!SockOk(port->fd)) {
            Log("net: P2P port: no UDP socket (%d)", SockError());
            return nullptr;
        }
#if defined(_WIN32)
        const int exclusive = 1;
        ::setsockopt(port->fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = Bswap16(at);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (::bind(port->fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == 0) {
            port->port = at;
            break;
        }
        const bool in_use = SockAddrInUse();
        const int error = SockError();
        SockClose(port->fd);
        port->fd = kBadSock;
        if (!in_use) {
            Log("net: P2P port %u: bind failed (%d)", at, error);
            return nullptr;
        }
        Log("net: P2P port %u is in use by another program%s", at,
            i + 1 < kTries ? "; trying the next" : "");
    }
    if (!SockOk(port->fd)) return nullptr;
    SockSetNonblock(port->fd);
    if (port->port != want) {
        Log("net: P2P port %u (BB_HD_P2P_PORT %u was taken); peers reach this game there",
            port->port, want);
    } else {
        Log("net: P2P port %u (udp)", port->port);
    }
    SetP2pPortBound(port->port);
    port->last_report = std::chrono::steady_clock::now();
    // The reader holds the port for the life of the process.
    std::thread(Reader, port).detach();
    g_port = port;
    return port;
}

} // namespace

bool P2pOpen() {
    return PortOpen() != nullptr;
}

void P2pSetDeliver(P2pDeliver deliver) {
    g_deliver.store(deliver, std::memory_order_release);
}

std::uint16_t P2pBoundPort() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_port ? g_port->port : 0;
}

int P2pSend(std::uint16_t src_vport, std::uint16_t dst_vport, const void* data, std::uint32_t len,
            std::uint32_t addr, std::uint16_t port_nbo) {
    const std::shared_ptr<Port> port = PortOpen();
    if (!port || (!data && len) || len > kMaxUdpPayload - kMaxHeader - kRelayHeader) return -1;
    // The relay's frame, then the P2P header, then the game's bytes: one buffer, sent from
    // the relay frame's start or from the header's.
    std::vector<std::uint8_t> frame(kRelayHeader + kMaxHeader + len);
    std::uint8_t* const pkt = frame.data() + kRelayHeader;
    std::size_t hdr = 0;
    pkt[0] = kP2pMagic;
    if (src_vport < 256 && dst_vport < 256) {
        pkt[1] = kP2pFlagP2p | kP2pFlagByteVports | kP2pType;
        pkt[2] = static_cast<std::uint8_t>(src_vport);
        pkt[3] = static_cast<std::uint8_t>(dst_vport);
        hdr = 4;
    } else {
        pkt[1] = kP2pFlagP2p | kP2pType;
        pkt[2] = static_cast<std::uint8_t>(src_vport >> 8);
        pkt[3] = static_cast<std::uint8_t>(src_vport);
        pkt[4] = static_cast<std::uint8_t>(dst_vport >> 8);
        pkt[5] = static_cast<std::uint8_t>(dst_vport);
        hdr = 6;
    }
    if (len) std::memcpy(pkt + hdr, data, len);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = port_nbo;
    sa.sin_addr.s_addr = addr;
    int n;
    std::uint8_t token[Stun::kTokenLen];
    sockaddr_in stun_sa{};
    if (RelayTarget(addr, port_nbo, token, &stun_sa)) {
        std::uint8_t* const f = frame.data();
        f[0] = kRelayMagic;
        f[1] = kRelayToServer;
        std::memcpy(f + 2, token, Stun::kTokenLen);
        std::memcpy(f + 10, &port_nbo, 2); // network order is the frame's big-endian
        n = SendTo(port->fd, f, kRelayHeader + hdr + len, stun_sa);
    } else {
        n = SendTo(port->fd, pkt, hdr + len, sa);
    }
    if (Cfg().trace || g_send_logs.fetch_add(1) < kTraceFirst) {
        char ip[32];
        WriteIpv4(ip, sizeof(ip), addr);
        Log("net: send -> %s:%u vport %u->%u: %u bytes -> %d", ip, Bswap16(port_nbo), src_vport,
            dst_vport, len, n);
    }
    if (n < 0) return -1;
    {
        std::lock_guard<std::mutex> lk(port->mu);
        ++port->tx;
    }
    return static_cast<int>(len);
}

// --- The reflexive address and hole punching ---------------------------------

bool P2pStun(const char* host, std::uint16_t sport, int timeout_ms, std::uint32_t* mapped_addr,
             std::uint16_t* mapped_port, Stun::Relay* relay) {
    const std::shared_ptr<Port> port = PortOpen();
    if (!port) {
        Log("stun: no P2P port %d to ask from", Cfg().p2p_port);
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) {
        Log("stun: cannot resolve %s", host);
        return false;
    }
    sockaddr_in sa{};
    std::memcpy(&sa, res->ai_addr, sizeof(sa));
    sa.sin_port = Bswap16(sport);
    ::freeaddrinfo(res);
    // With `relay`, the HELLO: the token this server gave, else zeros.
    std::uint8_t token[Stun::kTokenLen] = {};
    bool have_token = false;
    if (relay) {
        std::lock_guard<std::mutex> rl(g_relay.mu);
        if (g_relay.on && g_relay.server == sa.sin_addr.s_addr && g_relay.stun_port == sa.sin_port) {
            std::memcpy(token, g_relay.token, sizeof(token));
            have_token = true;
        }
    }
    std::uint8_t req[Stun::kMaxRequest];
    std::unique_lock<std::mutex> lk(port->mu);
    if (port->stun_pending) return false; // one at a time
    const std::size_t req_len = Stun::BuildBindingRequest(req, port->stun_txid, relay != nullptr,
                                                          have_token ? token : nullptr);
    port->stun_pending = true;
    port->stun_done = false;
    lk.unlock();
    (void)SendTo(port->fd, req, req_len, sa);
    lk.lock();
    port->stun_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return port->stun_done; });
    const bool ok = port->stun_done;
    port->stun_pending = false;
    if (!ok) return false;
    if (mapped_addr) *mapped_addr = port->stun_addr;
    if (mapped_port) *mapped_port = port->stun_port;
    if (relay) {
        *relay = port->stun_relay;
        std::lock_guard<std::mutex> rl(g_relay.mu);
        const bool was = g_relay.on;
        const std::uint16_t was_vport = g_relay.vport;
        if (relay->present) {
            g_relay.on = true;
            g_relay.server = sa.sin_addr.s_addr;
            g_relay.stun_port = sa.sin_port;
            std::memcpy(g_relay.token, relay->token, sizeof(g_relay.token));
            g_relay.vport = relay->vport;
        } else {
            g_relay.on = false; // a server without the relay: datagrams go as addressed
        }
        if (g_relay.on != was || g_relay.vport != was_vport) {
            char ip[32];
            WriteIpv4(ip, sizeof(ip), sa.sin_addr.s_addr);
            if (g_relay.on) {
                Log("net: relay on: %s:%u is our port %u; datagrams for its other ports go "
                    "through it",
                    ip, sport, g_relay.vport);
            } else {
                Log("net: relay off (%s:%u offers none)", ip, sport);
            }
        }
    }
    return true;
}

bool P2pRelay(std::uint32_t* server, std::uint16_t* vport) {
    std::lock_guard<std::mutex> lk(g_relay.mu);
    if (server) *server = g_relay.server;
    if (vport) *vport = g_relay.vport;
    return g_relay.on;
}

void P2pPunch(const char* label, std::uint32_t addr, std::uint16_t port_host,
              std::uint32_t local_addr, std::uint16_t local_port) {
    std::shared_ptr<Port> port;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        port = g_port;
    }
    if (!port || !addr || !port_host) return;
    {
        std::uint8_t token[Stun::kTokenLen];
        sockaddr_in stun_sa{};
        if (RelayTarget(addr, Bswap16(port_host), token, &stun_sa)) return; // the relay needs no hole
    }
    std::vector<sockaddr_in> targets;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = addr;
    a.sin_port = Bswap16(port_host);
    targets.push_back(a);
    if (local_addr && local_port && (local_addr != addr || local_port != port_host)) {
        sockaddr_in b{};
        b.sin_family = AF_INET;
        b.sin_addr.s_addr = local_addr;
        b.sin_port = Bswap16(local_port);
        targets.push_back(b);
    }
    const std::string name = label ? label : "peer";
    std::thread([port, targets, name] {
        const auto start = std::chrono::steady_clock::now();
        int sent = 0;
        bool heard = false;
        // Anything from either address since the start: the peer is through.
        const auto heard_since_start = [&] {
            std::lock_guard<std::mutex> lk(port->mu);
            for (const sockaddr_in& t : targets) {
                const auto it = port->heard.find(SourceKey(t.sin_addr.s_addr, t.sin_port));
                if (it != port->heard.end() && it->second >= start) return true;
            }
            return false;
        };
        for (int i = 0; i < 40 && !port->stop.load(std::memory_order_relaxed); ++i) {
            for (const sockaddr_in& t : targets) {
                if (SendTo(port->fd, kProbe, sizeof(kProbe), t) >= 0) ++sent;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (heard_since_start()) {
                heard = true;
                break;
            }
        }
        char ip[32];
        WriteIpv4(ip, sizeof(ip), targets[0].sin_addr.s_addr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        Log("net: punch %s %s:%u%s: %d probes, %s after %lld ms", name.c_str(), ip,
            Bswap16(targets[0].sin_port), targets.size() > 1 ? " (+local)" : "", sent,
            heard ? "peer heard" : "nothing heard", static_cast<long long>(ms));
    }).detach();
}

} // namespace Hd
