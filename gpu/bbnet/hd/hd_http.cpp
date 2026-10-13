// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/net/http.cpp (libcurl there, cpp-httplib here, as the game's own
// requests in core/libraries/network/http.cpp).
#include "hd/hd_http.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>

#include <httplib.h>

#include "hd/hd.h"
#include "hd/hd_account.h"
#include "hd/hd_engine.h"

namespace Hd {

namespace {

// This port says who it is: the server's operators tell clients apart by this.
constexpr const char* kUserAgent = "bloodborne_pc (bbhost online protocol)";

bool SameHost(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

#if defined(_WIN32) && defined(CPPHTTPLIB_OPENSSL_SUPPORT)
// The certificates this PC trusts (the Windows "ROOT" and "CA" stores cpp-httplib also
// reads), as PEM, without the ones that are not valid today.
//
// cpp-httplib copies the stores into OpenSSL as they are, and OpenSSL prefers a certificate
// it finds in its trust store to the one the server sends. A PC that still holds an old,
// expired copy of an intermediate (Windows keeps the ones it has seen; Let's Encrypt's
// cross-signed roots were reissued) then refuses a server whose own chain is valid:
// "certificate has expired". Without the expired entries OpenSSL takes the server's valid
// one. (launcher/bbport_hd.py does the same for the account calls.)
const std::string& TrustedPem() {
    static const std::string pem = [] {
        std::string out;
        int kept = 0, left_out = 0;
        for (const wchar_t* name : {L"ROOT", L"CA"}) {
            const HCERTSTORE store = CertOpenSystemStoreW(0, name);
            if (!store) continue;
            PCCERT_CONTEXT cert = nullptr;
            while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
                if (CertVerifyTimeValidity(nullptr, cert->pCertInfo) != 0) {
                    ++left_out; // expired, or not valid yet
                    continue;
                }
                DWORD size = 0;
                if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded,
                                          CRYPT_STRING_BASE64HEADER, nullptr, &size)) {
                    continue;
                }
                std::string text(size, '\0');
                if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded,
                                          CRYPT_STRING_BASE64HEADER, text.data(), &size)) {
                    continue;
                }
                text.resize(size);
                out += text;
                ++kept;
            }
            CertCloseStore(store, 0);
        }
        Log("net: HTTPS trusts %d certificates of this PC's stores; %d that are not valid today "
            "are left out",
            kept, left_out);
        return out;
    }();
    return pem;
}
#endif

HttpResult Request(const std::string& url, const std::string* post, int timeout_ms);

HttpResult Perform(const std::string& url, const std::string* post, int timeout_ms) {
    const auto started = std::chrono::steady_clock::now();
    HttpResult r = Request(url, post, timeout_ms);
    r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 started)
               .count();
    return r;
}

HttpResult Request(const std::string& url, const std::string* post, int timeout_ms) {
    HttpResult r;
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        r.error = "bad server address";
        return r;
    }
    const std::size_t slash = url.find('/', scheme_end + 3);
    const std::string scheme = url.substr(0, scheme_end);
    const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    // host[:port]
    std::string host = url.substr(scheme_end + 3, slash == std::string::npos
                                                      ? std::string::npos
                                                      : slash - scheme_end - 3);
    int port = scheme == "https" ? 443 : 80;
    if (const std::size_t colon = host.rfind(':'); colon != std::string::npos) {
        port = std::atoi(host.c_str() + colon + 1);
        host = host.substr(0, colon);
    }
    if (host.empty() || port <= 0 || port > 65535 || (scheme != "http" && scheme != "https")) {
        r.error = "bad server address";
        return r;
    }
    try {
        std::unique_ptr<httplib::ClientImpl> client;
        if (scheme == "https") {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            auto tls = std::make_unique<httplib::SSLClient>(host, port);
            tls->enable_server_certificate_verification(Cfg().verify_tls);
            // Closing does not wait for the server's own goodbye: after a call the server
            // did not answer, that wait is the read timeout once more (the server's nginx
            // does not read from us while its backend is busy), with no poll in the air.
            // The replies carry their length, so nothing is cut short unnoticed.
            SSL_CTX_set_quiet_shutdown(static_cast<SSL_CTX*>(tls->tls_context()), 1);
#if defined(_WIN32)
            // With a store of ours the library does not add the system's certificates.
            if (Cfg().verify_tls) {
                if (const std::string& pem = TrustedPem(); !pem.empty()) {
                    tls->load_ca_cert_store(pem.data(), pem.size());
                }
            }
#endif
            client = std::move(tls);
#else
            r.error = "no TLS in this build";
            return r;
#endif
        } else {
            client = std::make_unique<httplib::ClientImpl>(host, port);
        }
        httplib::ClientImpl& cli = *client;
        if (!cli.is_valid()) {
            r.error = "bad server address";
            return r;
        }
        const auto timeout = std::chrono::milliseconds(timeout_ms < 1 ? 1 : timeout_ms);
        cli.set_connection_timeout(timeout);
        cli.set_read_timeout(timeout);
        cli.set_write_timeout(timeout);
        cli.set_follow_location(false);
        httplib::Headers headers;
        headers.emplace("Accept", "application/json");
        headers.emplace("User-Agent", kUserAgent);
        // The account's token names us to the server...
        if (const std::string token = AccountToken(); !token.empty()) {
            headers.emplace("X-BB-Token", token);
        }
        // ... and the rules this session plays by, as on the game's own requests.
        headers.emplace("X-BBHost-Ruleset", Cfg().ruleset);
        const std::string json_type = "application/json";
        auto res = post ? cli.Post(path, headers, *post, json_type) : cli.Get(path, headers);
        if (!res) {
            r.error = httplib::to_string(res.error());
            return r;
        }
        r.status = res->status;
        r.body = res->body;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

bool ParseReply(const std::string& what, const HttpResult& r, Json& out, std::string& error) {
    static std::atomic<int> logs{0};
    if (!r.error.empty()) {
        error = r.error;
    } else if (r.status < 200 || r.status >= 300) {
        error = "HTTP " + std::to_string(r.status);
        // 401 on the NP surfaces is the server refusing the sign-in, not a passing failure
        // (the server's account-token check): say so once and let the poller slow down.
        if (r.status == 401) {
            const Json body = Json::parse(r.body, nullptr, false);
            const std::string why = body.is_discarded() ? std::string() : StrOf(body, "Message");
            AccountMarkRefused(why.empty() ? "HTTP 401" : why);
        }
    } else {
        out = Json::parse(r.body, nullptr, false);
        if (!out.is_discarded()) return true;
        out = Json();
        error = "bad JSON";
    }
    // With the time it took: a server that does not answer (the call's whole timeout) reads
    // differently in a log than one that hangs up at once.
    if (logs.fetch_add(1) < 16) {
        Log("np: %s -> %s (after %lld ms)", what.c_str(), error.c_str(), r.ms);
    }
    return false;
}

} // namespace

const std::string& TrustedCaPem() {
#if defined(_WIN32) && defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    return TrustedPem();
#else
    static const std::string none;
    return none;
#endif
}

std::string NpServerBase() {
    const Config& c = Cfg();
    if (!c.np_server.empty()) return c.np_server;
    return c.scheme + "://" + c.host + ":18671";
}

std::string AuthServerBase() {
    const Config& c = Cfg();
    if (!c.auth_server.empty()) return c.auth_server;
    return c.scheme + "://" + c.host;
}

std::string HostOfUrl(const std::string& url) {
    std::string host = url;
    if (const std::size_t s = host.find("://"); s != std::string::npos) host = host.substr(s + 3);
    host = host.substr(0, host.find('/'));
    host = host.substr(0, host.find(':'));
    return host;
}

HttpResult HttpGet(const std::string& url, int timeout_ms) {
    return Perform(url, nullptr, timeout_ms);
}

HttpResult HttpPostJson(const std::string& url, const Json& body, int timeout_ms) {
    const std::string text = body.dump();
    return Perform(url, &text, timeout_ms);
}

// How long a session call waits for its answer. bbhost gives these calls 3 to 5 s. In play
// from far away the server was seen to answer later than that again and again (polls and
// heartbeats given up after 4 and 3 s while the game's own requests, which wait two minutes,
// all arrived): a call given up is an event not heard or a heartbeat not counted, and asking
// again starts from nothing. So wait at least BB_HD_ANSWER_SECONDS (15); 0 keeps bbhost's.
static int Patient(int timeout_ms) {
    int seconds = Cfg().answer_seconds;
    seconds = seconds < 0 ? 0 : seconds > 120 ? 120 : seconds;
    return timeout_ms < seconds * 1000 ? seconds * 1000 : timeout_ms;
}

bool NpPost(const std::string& path, const Json& body, Json& out, std::string& error,
            int timeout_ms) {
    const std::string url = NpServerBase() + path;
    return ParseReply("POST " + path, HttpPostJson(url, body, Patient(timeout_ms)), out, error);
}

bool NpGet(const std::string& path, Json& out, std::string& error, int timeout_ms) {
    const std::string url = NpServerBase() + path;
    return ParseReply("GET " + path, HttpGet(url, Patient(timeout_ms)), out, error);
}

std::string StrOf(const Json& obj, const char* key, const std::string& dflt) {
    if (!obj.is_object()) return dflt;
    const auto it = obj.find(key);
    if (it == obj.end()) return dflt;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(it->get<double>()));
        return buf;
    }
    return dflt;
}

long long IntOf(const Json& obj, const char* key, long long dflt) {
    if (!obj.is_object()) return dflt;
    const auto it = obj.find(key);
    if (it == obj.end()) return dflt;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number()) return static_cast<long long>(it->get<double>());
    if (it->is_string()) return std::strtoll(it->get<std::string>().c_str(), nullptr, 10);
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    return dflt;
}

std::vector<std::pair<std::string, std::string>> GameRequestHeaders(const std::string& host) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!Enabled()) return out;
    // bbhost sends these on every request the game makes, all of which reach the private
    // server there. Here a host override may point elsewhere, and the token is the account:
    // it goes to the private server only.
    if (!SameHost(host, Cfg().host) && !SameHost(host, HostOfUrl(NpServerBase()))) return out;
    // Before the request leaves: a character without an id from the server asks for one at
    // the login's next step instead of sending the "none" marker as its id (hd_engine.h).
    Engine::FixCharaId();
    if (const std::string token = AccountToken(); !token.empty()) {
        out.emplace_back("X-BB-Token", token);
    }
    out.emplace_back("X-BBHost-Ruleset", Cfg().ruleset);
    return out;
}

} // namespace Hd
