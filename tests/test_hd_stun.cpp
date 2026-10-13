// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: tests/stun_test.cpp.
//
// gpu/bbnet/hd/hd_stun.cpp: the Binding Request's shape, and the response as the private
// server's STUN listener (RFC 3489 attributes, XOR with the transaction id) and an RFC 5389
// server (XOR with the cookie, no MAPPED-ADDRESS) send it.
// Build and run: ninja -C out/gpu hd-stun-test && out/gpu/hd-stun-test
#include "hd/hd_stun.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                               \
            ++g_fail;                                                                              \
        }                                                                                          \
    } while (0)

void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}
void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    Put16(v, static_cast<std::uint16_t>(x >> 16));
    Put16(v, static_cast<std::uint16_t>(x));
}
std::uint32_t Rd32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
std::uint16_t Rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

// A response with the given attributes (type, address host order, port, xored with the
// transaction id or plain).
using Attr = std::tuple<std::uint16_t, std::uint32_t, std::uint16_t, bool>;
std::vector<std::uint8_t> Response(const std::uint8_t txid[16], const std::vector<Attr>& attrs) {
    std::vector<std::uint8_t> body;
    for (const auto& [type, addr, port, xored] : attrs) {
        Put16(body, type);
        Put16(body, 8);
        body.push_back(0);
        body.push_back(1);
        Put16(body, xored ? static_cast<std::uint16_t>(port ^ Rd16(txid)) : port);
        Put32(body, xored ? (addr ^ Rd32(txid)) : addr);
    }
    std::vector<std::uint8_t> out;
    Put16(out, 0x0101);
    Put16(out, static_cast<std::uint16_t>(body.size()));
    out.insert(out.end(), txid, txid + 16);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::uint32_t Nbo(std::uint32_t host) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(host >> 24),
                               static_cast<std::uint8_t>(host >> 16),
                               static_cast<std::uint8_t>(host >> 8), static_cast<std::uint8_t>(host)};
    std::uint32_t v;
    std::memcpy(&v, b, 4);
    return v;
}

} // namespace

int main() {
    using namespace Hd::Stun;
    std::uint8_t req[kMaxRequest], txid[16];
    CHECK(BuildBindingRequest(req, txid) == 20);
    CHECK(Rd16(req) == 0x0001);
    CHECK(Rd16(req + 2) == 0);
    CHECK(std::memcmp(req + 4, txid, 16) == 0);
    CHECK(Rd32(txid) == 0x2112A442u); // the cookie leads the id
    std::uint8_t txid2[16], req2[kMaxRequest];
    BuildBindingRequest(req2, txid2);
    CHECK(std::memcmp(txid + 4, txid2 + 4, 12) != 0); // ids differ

    // The relay HELLO: "bbr1" and the token (zeros before the first).
    std::uint8_t hreq[kMaxRequest], htxid[16];
    const std::uint8_t tok[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(BuildBindingRequest(hreq, htxid, true, nullptr) == 36);
    CHECK(Rd16(hreq + 2) == 16 && Rd16(hreq + 20) == 0x8100 && Rd16(hreq + 22) == 12);
    CHECK(std::memcmp(hreq + 24, "bbr1", 4) == 0);
    CHECK(hreq[28] == 0 && hreq[35] == 0);
    BuildBindingRequest(hreq, htxid, true, tok);
    CHECK(std::memcmp(hreq + 28, tok, 8) == 0);

    const std::uint32_t ip = 0xc0a80105u; // 192.168.1.5
    std::uint32_t addr = 0;
    std::uint16_t port = 0;
    // The private server: MAPPED, SOURCE, CHANGED, then XOR-MAPPED (0x8020) xored with the id.
    auto r = Response(txid, {{0x0001, ip, 9307, false},
                             {0x0004, 0x0a000001u, 3478, false},
                             {0x0005, 0x0a000001u, 3479, false},
                             {0x8020, ip, 9307, true}});
    CHECK(ParseBindingResponse(r.data(), r.size(), txid, &addr, &port));
    CHECK(addr == Nbo(ip));
    CHECK(port == 9307);
    // RFC 5389: only XOR-MAPPED-ADDRESS (0x0020), xored with the cookie (= the id's first
    // bytes here).
    addr = port = 0;
    r = Response(txid, {{0x0020, 0x5db8d822u, 41234, true}});
    CHECK(ParseBindingResponse(r.data(), r.size(), txid, &addr, &port));
    CHECK(addr == Nbo(0x5db8d822u));
    CHECK(port == 41234);
    // Another transaction's response, a request, a truncated response: refused.
    CHECK(!ParseBindingResponse(r.data(), r.size(), txid2, &addr, &port));
    CHECK(!ParseBindingResponse(req, 20, txid, &addr, &port));
    CHECK(!ParseBindingResponse(r.data(), r.size() - 4, txid, &addr, &port));
    // A response with no address attribute.
    r = Response(txid, {{0x0004, 0x0a000001u, 3478, false}});
    CHECK(!ParseBindingResponse(r.data(), r.size(), txid, &addr, &port));

    // The relay's answer: MAPPED-ADDRESS is the relay port; BBHOST-RELAY carries the token,
    // the port and where the request came from.
    r = Response(txid, {{0x0001, 0xa7ac71dbu, 4123, false}});
    std::vector<std::uint8_t> rel = {0x81, 0x01, 0, 20, 'b', 'b', 'r', '1', 9, 8, 7, 6, 5, 4, 3, 2};
    Put16(rel, 4123);
    Put32(rel, 0x6cd2a10eu);
    Put16(rel, 51000);
    r.insert(r.end(), rel.begin(), rel.end());
    r[2] = static_cast<std::uint8_t>((r.size() - 20) >> 8);
    r[3] = static_cast<std::uint8_t>(r.size() - 20);
    Relay relay;
    CHECK(ParseBindingResponse(r.data(), r.size(), txid, &addr, &port, &relay));
    CHECK(addr == Nbo(0xa7ac71dbu) && port == 4123);
    CHECK(relay.present && relay.vport == 4123 && relay.token[0] == 9 && relay.token[7] == 2);
    CHECK(relay.observed_addr == Nbo(0x6cd2a10eu) && relay.observed_port == 51000);
    // Without the attribute (a server without the relay): not present.
    r = Response(txid, {{0x0001, ip, 9307, false}});
    CHECK(ParseBindingResponse(r.data(), r.size(), txid, &addr, &port, &relay));
    CHECK(!relay.present);

    std::printf(g_fail ? "hd-stun-test: %d failures\n" : "hd-stun-test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
