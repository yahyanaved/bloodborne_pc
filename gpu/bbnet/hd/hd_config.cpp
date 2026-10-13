// SPDX-License-Identifier: GPL-3.0-or-later
// The settings of the Hunter's Dream backend, read once from the environment the launcher
// and run.py set. The names and defaults are bbhost's [online] section (src/core/config.cpp).
#include "hd/hd.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Hd {

namespace {

std::string Env(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name);
    return v && v[0] ? v : fallback;
}

bool Flag(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return fallback;
    return v[0] == '1' || v[0] == 'y' || v[0] == 't';
}

int Number(const char* name, int fallback) {
    const char* v = std::getenv(name);
    if (!v || !v[0]) return fallback;
    char* end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return end == v ? fallback : static_cast<int>(n);
}

// The rules a session announces are short and of [a-z0-9_.;=,-] (bbhost's plugins_enter_world
// takes no others from the server); anything else from the environment is not sent.
std::string CleanRuleset(const std::string& text) {
    if (text.empty() || text.size() > 256) return "vanilla";
    for (const char ch : text) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                        ch == '.' || ch == ';' || ch == '=' || ch == ',' || ch == '-';
        if (!ok) return "vanilla";
    }
    return text;
}

Config Load() {
    Config c;
    const std::string backend = Env("BB_ONLINE_BACKEND");
    c.enabled = Flag("BB_ONLINE", false) &&
                (backend == "hd" || backend == "huntersdream" || backend == "bbhost");

    // BB_COMMUNITY_SERVER is "https://host" (the launcher's community_address).
    std::string community = Env("BB_COMMUNITY_SERVER", "https://thehuntersdream.com");
    if (const std::size_t s = community.find("://"); s != std::string::npos) {
        c.scheme = community.substr(0, s);
        community = community.substr(s + 3);
    }
    community = community.substr(0, community.find('/'));
    community = community.substr(0, community.find(':'));
    if (!community.empty()) c.host = community;
    if (c.scheme != "http" && c.scheme != "https") c.scheme = "https";

    c.np_server = Env("BB_HD_NP_SERVER");
    c.auth_server = Env("BB_HD_AUTH_SERVER");
    for (std::string* base : {&c.np_server, &c.auth_server}) {
        while (!base->empty() && base->back() == '/') base->pop_back();
    }
    c.verify_tls = Flag("BB_HD_VERIFY_TLS", true);
    c.require_account = Flag("BB_HD_REQUIRE_ACCOUNT", true);
    c.online_id = Env("BB_HD_ONLINE_ID");
    c.account_name = Env("BB_HD_ACCOUNT_NAME");
    c.account_token = Env("BB_HD_ACCOUNT_TOKEN");
    c.stun_server = Env("BB_HD_STUN_SERVER");
    c.p2p_addr = Env("BB_HD_P2P_ADDR");
    c.p2p_port = Number("BB_HD_P2P_PORT", 9307);
    if (c.p2p_port <= 0 || c.p2p_port > 65535) c.p2p_port = 9307;
    c.relay = Flag("BB_HD_RELAY", true);
    c.sign_timeout_seconds = Number("BB_HD_SIGN_TIMEOUT", 30);
    c.answer_seconds = Number("BB_HD_ANSWER_SECONDS", 15);
    c.playlog_upload_seconds = Number("BB_HD_PLAYLOG_SECONDS", 5);
    c.playlog_sample_ms = Number("BB_HD_PLAYLOG_SAMPLE_MS", 500);
    c.ruleset = CleanRuleset(Env("BB_HD_RULESET", "vanilla"));
    c.eboot_109 = Flag("BB_EBOOT_109", false);
    c.trace = Flag("BB_HD_TRACE", false);
    return c;
}

} // namespace

const Config& Cfg() {
    static const Config c = Load();
    return c;
}

void Log(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    std::printf("Co-op: %s\n", line);
}

} // namespace Hd

extern "C" int bbnet_hd_enabled(void) {
    return Hd::Enabled() ? 1 : 0;
}
