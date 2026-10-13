// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/net/http.h.
//
// A small synchronous HTTP client for the backend's own traffic to the private server (the
// session service). The game's own sceHttp calls go through core/libraries/network/http.cpp;
// this one never touches guest state and may be used from any host thread.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace Hd {

using Json = nlohmann::json;

struct HttpResult {
    long status = 0;   // HTTP status, 0 when the transport failed
    std::string body;
    std::string error; // transport error text, empty on success
    long long ms = 0;  // how long the request took, answered or not
    bool ok() const {
        return error.empty() && status >= 200 && status < 300;
    }
};

// BB_HD_NP_SERVER, else "<scheme>://<host>:18671".
std::string NpServerBase();
// BB_HD_AUTH_SERVER, else "<scheme>://<host>": the origin the account calls go to.
std::string AuthServerBase();
// The host of a "scheme://host[:port][/...]" text.
std::string HostOfUrl(const std::string& url);

HttpResult HttpGet(const std::string& url, int timeout_ms = 5000);
HttpResult HttpPostJson(const std::string& url, const Json& body, int timeout_ms = 5000);

// POST/GET to "<NpServerBase><path>" and parse the JSON reply. Returns false (with `error`)
// on a transport error, a non-2xx status, or a body that is not JSON; `out` holds the
// parsed object on success.
bool NpPost(const std::string& path, const Json& body, Json& out, std::string& error,
            int timeout_ms = 5000);
bool NpGet(const std::string& path, Json& out, std::string& error, int timeout_ms = 5000);

// Reads that never throw: the member or a default.
std::string StrOf(const Json& obj, const char* key, const std::string& dflt = "");
long long IntOf(const Json& obj, const char* key, long long dflt = 0);

} // namespace Hd
