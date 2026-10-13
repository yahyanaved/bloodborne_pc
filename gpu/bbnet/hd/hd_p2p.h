// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/hle/net_p2p.h and the P2P half of src/hle/net.cpp.
//
// The instance's P2P UDP port with bbhost's wire on it. Every game socket of type
// SOCK_DGRAM_P2P shares the port (core/libraries/network/p2p_sockets.cpp registers them by
// vport); this file frames what they send, takes apart what arrives, and gives the session
// layer what it needs for players behind NAT: the reflexive address of the port through a
// STUN Binding exchange on the port's own socket, hole-punch probes toward a peer so this
// side's NAT opens for the peer's datagrams before the game's own traffic starts, and the
// private server's relay.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hd/hd_stun.h"

namespace Hd {

// A datagram for the game from the P2P port, the wire's header taken off.
struct P2pDatagram {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint32_t addr = 0;      // the sender, network byte order
    std::uint16_t port = 0;      // network byte order
    std::uint16_t src_vport = 0; // host order
    std::uint16_t dst_vport = 0; // host order
    bool has_header = false;     // false: no P2P header, the first bound vport takes it
};
// Called on the port's reader thread for every datagram that is the game's.
using P2pDeliver = void (*)(const P2pDatagram& datagram);

// Opens the port (BB_HD_P2P_PORT, or the next free one of 16) and starts its reader; true
// when it is open. The port stays open for the session.
bool P2pOpen();
void P2pSetDeliver(P2pDeliver deliver);
// The UDP port the instance bound, host order; 0 before P2pOpen.
std::uint16_t P2pBoundPort();

// Sends one datagram of the game's from vport `src_vport` to addr:port (network byte order)
// vport `dst_vport`, with the P2P header in front and through the relay when the address is
// one of its ports. Returns len, or -1.
int P2pSend(std::uint16_t src_vport, std::uint16_t dst_vport, const void* data, std::uint32_t len,
            std::uint32_t addr, std::uint16_t port_nbo);

// Sends a STUN Binding Request from the P2P port (opened if the game has not bound it yet)
// to host:port and waits up to timeout_ms for the response. True with the mapped address
// (network byte order) and port (host order). With `relay`, the request carries the relay
// HELLO (hd_stun.h) and the answer's BBHOST-RELAY lands there; a relay in the answer turns
// framing on for datagrams to that server's other ports, an answer without one off.
bool P2pStun(const char* host, std::uint16_t port, int timeout_ms, std::uint32_t* mapped_addr,
             std::uint16_t* mapped_port, Stun::Relay* relay = nullptr);

// Whether datagrams go through the server's relay: its address (network order) and our
// port on it.
bool P2pRelay(std::uint32_t* server, std::uint16_t* vport);

// Starts sending small probe datagrams from the P2P port to addr:port and, when different,
// local_addr:local_port (network byte order, host order), every 500 ms for up to 20 s or
// until anything arrives from either. The probes carry their own magic and the receiving
// port drops them; the game never sees them. `label` names the peer in the log. A peer
// behind the relay gets none: the relay needs no hole.
void P2pPunch(const char* label, std::uint32_t addr, std::uint16_t port, std::uint32_t local_addr,
              std::uint16_t local_port);

} // namespace Hd
