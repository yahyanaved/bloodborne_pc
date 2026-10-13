// SPDX-License-Identifier: GPL-3.0-or-later
// The Hunter's Dream online backend: the online implementation of bbhost
// (github.com/droogie/bbhost, GPL-3.0-or-later, by droogie and its contributors) ported to
// this port's runtime. With BB_ONLINE=1 and BB_ONLINE_BACKEND=hd the game's PSN calls are
// answered here instead of by the shadNet libraries: NpMatching2 and NpSignaling run over
// the private server's session service (JSON over HTTPS), and the P2P socket speaks bbhost's
// wire (the PS4 vport header, a STUN binding for the reflexive address, hole-punch probes,
// the server's relay), so players on this port and bbhost players meet in the same rooms.
//
// Every file in this folder is a port of the bbhost source named at its top.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Hd {

struct Config {
    bool enabled = false;            // BB_ONLINE=1 and BB_ONLINE_BACKEND=hd
    std::string scheme = "https";    // of BB_COMMUNITY_SERVER
    std::string host = "thehuntersdream.com";
    std::string np_server;           // BB_HD_NP_SERVER: the API base, else <scheme>://<host>:18671
    std::string auth_server;         // BB_HD_AUTH_SERVER: the account calls, else <scheme>://<host>
    bool verify_tls = true;          // BB_HD_VERIFY_TLS
    bool require_account = true;     // BB_HD_REQUIRE_ACCOUNT: signed out until an account is linked
    std::string online_id;           // BB_HD_ONLINE_ID: the name without an account
    std::string account_name;        // BB_HD_ACCOUNT_NAME / BB_HD_ACCOUNT_TOKEN, from the launcher
    std::string account_token;
    std::string stun_server;         // BB_HD_STUN_SERVER: "host[:port]", "" = <host>:3478, "off"
    std::string p2p_addr;            // BB_HD_P2P_ADDR: the address reported as local (127.0.0.1)
    int p2p_port = 9307;             // BB_HD_P2P_PORT
    bool relay = true;               // BB_HD_RELAY=0: no relay HELLO in the STUN binding
    int sign_timeout_seconds = 30;   // BB_HD_SIGN_TIMEOUT (the game's own: 180; 0 keeps it)
    int answer_seconds = 15;         // BB_HD_ANSWER_SECONDS: least wait for a session call's answer (0: bbhost's 3-5 s)
    int playlog_upload_seconds = 5;  // BB_HD_PLAYLOG_SECONDS (the game's own: 300; 0 keeps it)
    int playlog_sample_ms = 500;     // BB_HD_PLAYLOG_SAMPLE_MS (the game's own: 1500; 0 keeps it)
    std::string ruleset = "vanilla"; // BB_HD_RULESET: what this session plays by (run.py)
    bool eboot_109 = false;          // BB_EBOOT_109: the 1.09 eboot the addresses were read from
    bool trace = false;              // BB_HD_TRACE
};

const Config& Cfg();
inline bool Enabled() {
    return Cfg().enabled;
}

// One "Co-op: ..." line on stdout (user\last_run.log). Never given a token.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// What the game's own requests to the private server carry besides its headers
// (core/libraries/network/http.cpp): the account's token and the session's rules.
// Empty for another host, or when this backend is off.
std::vector<std::pair<std::string, std::string>> GameRequestHeaders(const std::string& host);

// The certificates this PC trusts for HTTPS, as PEM, without the ones that are not valid today
// (hd_http.cpp). Empty where the HTTP library's own choice of certificates is right (not
// Windows). For every HTTPS client of the port, whichever co-op service is in use.
const std::string& TrustedCaPem();

// The export for an imported symbol's name ("sceNpMatching2JoinRoom"), or 0.
std::uintptr_t Resolve(const char* symbol);

} // namespace Hd

extern "C" {
// 1 when the Hunter's Dream backend answers the game's online calls.
int bbnet_hd_enabled(void);
// The backend's function for an imported symbol's name, or 0 (runtime_services.c).
std::uintptr_t bbnet_hd_resolve(const char* symbol);
// The loaded game image, before the game starts and while it is still writable (probe.c).
void bbnet_hd_set_image(void* image, std::uint64_t size);
}
