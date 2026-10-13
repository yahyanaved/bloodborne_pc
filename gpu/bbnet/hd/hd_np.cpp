// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: the NP part of src/hle/system.cpp (NpManager, NpAuth, NpWebApi,
// NpLookup, NpScore). The private server does not use PSN, so these succeed locally with
// the values the game expects: signed in as the account's name, PlayStation Plus present,
// an authorization code the server does not check (it knows the player by the X-BB-Token
// header), empty friend and block lists.
#include "hd/hd_np.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "hd/hd.h"
#include "hd/hd_account.h"
#include "hd/hd_engine.h"
#include "hd/hd_session.h"

namespace Hd {

namespace {

constexpr int kNpInvalidArg = static_cast<int>(0x80550003);

// ---- NpManager --------------------------------------------------------------

int PS4_SYSV_ABI np_restriction(int) {
    return 0;
}
int PS4_SYSV_ABI np_title(const void*, const void*) {
    return 0;
}
int PS4_SYSV_ABI np_reg_state(void*, void*) {
    Log("sceNpRegisterStateCallback");
    return 0;
}
int PS4_SYSV_ABI np_unreg_state(void*) {
    return 0;
}
int PS4_SYSV_ABI np_reg_presence(void*, void*) {
    Log("sceNpRegisterGamePresenceCallback");
    return 0;
}
int PS4_SYSV_ABI np_get_state(int, int* state) {
    if (!state) return kNpInvalidArg;
    // BB_HD_REQUIRE_ACCOUNT: signed out (1) until the launcher has linked an account, so
    // the game plays offline rather than as a name taken from a settings file. Otherwise
    // SIGNED_IN (2).
    *state = (Cfg().require_account && !AccountLoggedIn()) ? 1 : 2;
    return 0;
}
int PS4_SYSV_ABI np_online_id(int, void* id) {
    if (!id) return kNpInvalidArg;
    // The account's name is the name the game must carry in its signs and rooms, as the
    // session service already does through OnlineId().
    const std::string cur = OnlineId();
    std::memset(id, 0, 20);
    std::memcpy(id, cur.c_str(), std::min<std::size_t>(cur.size(), 16));
    return 0;
}
int PS4_SYSV_ABI np_get_npid(int, void* id) {
    if (!id) return kNpInvalidArg;
    // The same handle as sceNpGetOnlineId: the session service matches peers by it, so the
    // game's NpId and the server's online id must agree.
    NpFillNpId(id, OnlineId().c_str());
    return 0;
}
int PS4_SYSV_ABI np_presence_status(int, int* st) {
    if (st) *st = 1;
    return 0;
}
int PS4_SYSV_ABI np_cmp_online(const void* a, const void* b) {
    if (!a || !b) return kNpInvalidArg;
    return std::memcmp(a, b, 16);
}
int PS4_SYSV_ABI np_cmp_npid(const void* a, const void* b) {
    if (!a || !b) return kNpInvalidArg;
    return std::memcmp(a, b, 36);
}
// A no-op, as on shadPS4: the console kernel's sceNpCheckCallback also runs NP state
// management that latches FrpgNetMan's error flag; firing the callbacks is not needed
// because sceNpGetState already reports signed in.
int PS4_SYSV_ABI np_check_cb_noop() {
    return 0;
}

// ---- NP auth / availability / Plus / parental ---------------------------------

std::atomic<int> g_np_req{1};
int PS4_SYSV_ABI np_create_async_request(const void*) {
    return g_np_req.fetch_add(1);
}
int PS4_SYSV_ABI np_delete_request(int) {
    return 0;
}
int PS4_SYSV_ABI np_abort_request(int) {
    return 0;
}
int PS4_SYSV_ABI np_poll_async(int, int* result) {
    if (result) *result = 0;
    return 0; // finished
}
int PS4_SYSV_ABI np_check_availability(int, const void*, void*) {
    Engine::FixCharaId();
    return 0;
}
// sceNpGetParentalControlInfo(reqId, const SceNpOnlineId*, int8* age,
//                             SceNpParentalControlInfo* {bool content, chat, ugc})
int PS4_SYSV_ABI np_parental(int, const void*, std::int8_t* age, std::uint8_t* info) {
    if (age) *age = 25;
    if (info) std::memset(info, 0, 3);
    return 0;
}
// sceNpCheckPlus(reqId, const SceNpCheckPlusParameter*, SceNpCheckPlusResult* {bool authorized})
int PS4_SYSV_ABI np_check_plus(int, const void*, std::uint8_t* result) {
    if (result) result[0] = 1;
    Engine::FixCharaId();
    return 0;
}
int PS4_SYSV_ABI np_plus_cb(const void*, void*) {
    return 0;
}
int PS4_SYSV_ABI np_plus_notify(int, std::uint64_t) {
    return 0;
}
int PS4_SYSV_ABI npauth_create_async_request(const void*) {
    return g_np_req.fetch_add(1);
}
int PS4_SYSV_ABI npauth_delete_request(int) {
    return 0;
}
int PS4_SYSV_ABI npauth_poll_async(int, int* result) {
    if (result) *result = 0;
    return 0;
}
// sceNpAuthGetAuthorizationCode(reqId, const SceNpAuthGetAuthorizationCodeParameter*,
//                               SceNpAuthorizationCode* {char code[128]}, int* issuerId)
int PS4_SYSV_ABI npauth_get_code(int, const void*, char* code, int* issuer) {
    if (code) {
        std::memset(code, 0, 128);
        std::strncpy(code, "DUMMY", 127);
    }
    if (issuer) *issuer = 10;
    // The network login starts here.
    Engine::FixCharaId();
    return 0;
}

// ---- NpWebApi: the friend and block lists, empty ------------------------------

constexpr int kWebApiInvalid = static_cast<int>(0x80552902);
constexpr int kWebApiNoReq = static_cast<int>(0x80552906);
constexpr char kWebApiJsonType[] = "application/json; charset=utf-8";

struct WebApiReq {
    int ctx = 0;
    int status = 200;
    std::string body;
    std::size_t read_off = 0;
};

std::mutex g_web_mu;
int g_web_lib = 1;
int g_web_ctx = 1;
int g_web_filter = 1;
int g_web_cb = 1;
std::int64_t g_web_req = 1;
std::unordered_map<std::int64_t, WebApiReq> g_web_reqs;

std::string WebApiBodyForPath(const char* path) {
    if (path && std::strstr(path, "friendList")) return "{\"totalResults\":0,\"friendList\":[]}";
    if (path && std::strstr(path, "blockList")) return "{\"totalResults\":0,\"blockList\":[]}";
    return "{\"totalResults\":0}";
}

int PS4_SYSV_ABI webapi_init(int http_ctx, std::uint64_t pool) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    const int id = g_web_lib++;
    Log("sceNpWebApiInitialize http=%d pool=%llu -> %d", http_ctx,
        static_cast<unsigned long long>(pool), id);
    return id;
}
int PS4_SYSV_ABI webapi_term(int) {
    return 0;
}
int PS4_SYSV_ABI webapi_create_ctx(int lib, const void*) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    const int id = g_web_ctx++;
    Log("sceNpWebApiCreateContext lib=%d -> %d", lib, id);
    return id;
}
int PS4_SYSV_ABI webapi_delete_ctx(int) {
    return 0;
}
int PS4_SYSV_ABI webapi_create_filter(int lib, const void*, std::uint64_t n) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    const int id = g_web_filter++;
    Log("sceNpWebApiCreatePushEventFilter lib=%d n=%llu -> %d", lib,
        static_cast<unsigned long long>(n), id);
    return id;
}
int PS4_SYSV_ABI webapi_delete_filter(int, int) {
    return 0;
}
int PS4_SYSV_ABI webapi_reg_push(int ctx, int filter, void*, void*) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    const int id = g_web_cb++;
    Log("sceNpWebApiRegisterPushEventCallback ctx=%d filter=%d -> %d", ctx, filter, id);
    return id;
}
int PS4_SYSV_ABI webapi_unreg_push(int, int) {
    return 0;
}
int PS4_SYSV_ABI webapi_create_req(int ctx, const char* group, const char* path, int method,
                                   const void*, std::int64_t* req_id) {
    if (!path || !req_id) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    const std::int64_t id = g_web_req++;
    WebApiReq r{};
    r.ctx = ctx;
    r.body = WebApiBodyForPath(path);
    g_web_reqs[id] = std::move(r);
    *req_id = id;
    static int logs;
    if (logs < 8) {
        Log("sceNpWebApiCreateRequest ctx=%d group=%s path=%s method=%d -> %lld", ctx,
            group ? group : "", path, method, static_cast<long long>(id));
        ++logs;
    }
    return 0;
}
int PS4_SYSV_ABI webapi_delete_req(std::int64_t req) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    g_web_reqs.erase(req);
    return 0;
}
int PS4_SYSV_ABI webapi_abort_req(std::int64_t req) {
    return webapi_delete_req(req);
}
int PS4_SYSV_ABI webapi_send(std::int64_t req, const void*, std::uint64_t) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    return g_web_reqs.count(req) ? 0 : kWebApiNoReq;
}
int PS4_SYSV_ABI webapi_status(std::int64_t req, int* code) {
    if (!code) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    const auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    *code = it->second.status;
    return 0;
}
int PS4_SYSV_ABI webapi_read(std::int64_t req, void* data, std::uint64_t size) {
    std::lock_guard<std::mutex> lock(g_web_mu);
    const auto it = g_web_reqs.find(req);
    if (it == g_web_reqs.end()) return kWebApiNoReq;
    WebApiReq& r = it->second;
    if (!data || size == 0 || r.read_off >= r.body.size()) return 0;
    std::size_t n = static_cast<std::size_t>(size);
    const std::size_t left = r.body.size() - r.read_off;
    if (n > left) n = left;
    std::memcpy(data, r.body.data() + r.read_off, n);
    r.read_off += n;
    return static_cast<int>(n);
}
bool IsContentType(const char* name) {
    return std::strcmp(name, "Content-Type") == 0 || std::strcmp(name, "content-type") == 0;
}
int PS4_SYSV_ABI webapi_hdr_len(std::int64_t req, const char* name, std::uint64_t* len) {
    if (!name || !len) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (!IsContentType(name)) return kWebApiInvalid;
    *len = sizeof(kWebApiJsonType);
    return 0;
}
int PS4_SYSV_ABI webapi_hdr_val(std::int64_t req, const char* name, char* buf, std::uint64_t n) {
    if (!name || !buf || n == 0) return kWebApiInvalid;
    std::lock_guard<std::mutex> lock(g_web_mu);
    if (!g_web_reqs.count(req)) return kWebApiNoReq;
    if (!IsContentType(name)) return kWebApiInvalid;
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", kWebApiJsonType);
    return 0;
}
int PS4_SYSV_ABI webapi_parse_npid(const char* json, void* npid) {
    if (!json || !npid) return kWebApiInvalid;
    const char* s = json;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
    char online[17]{};
    const auto take_quoted = [&](const char* p) -> bool {
        const char* q = std::strchr(p, '"');
        if (!q) return false;
        ++q;
        const char* e = std::strchr(q, '"');
        if (!e) return false;
        const std::size_t n = static_cast<std::size_t>(e - q);
        if (n == 0 || n >= sizeof(online)) return false;
        std::memcpy(online, q, n);
        online[n] = 0;
        return true;
    };
    if (*s == '{') {
        const char* key = std::strstr(s, "onlineId");
        if (!key) key = std::strstr(s, "\"data\"");
        if (!key || !take_quoted(key)) return kWebApiInvalid;
    } else if (*s == '"') {
        if (!take_quoted(s)) return kWebApiInvalid;
    } else {
        std::snprintf(online, sizeof(online), "%s", s);
    }
    NpFillNpId(npid, online);
    return 0;
}

// ---- NpLookup -----------------------------------------------------------------

std::mutex g_lookup_mu;
int g_lookup_ctx = 1;
int g_lookup_req = 1;
int PS4_SYSV_ABI lookup_title_ctx(const void*) {
    std::lock_guard<std::mutex> lock(g_lookup_mu);
    const int id = g_lookup_ctx++;
    Log("sceNpLookupCreateTitleCtx -> %d", id);
    return id;
}
int PS4_SYSV_ABI lookup_delete_ctx(int) {
    return 0;
}
int PS4_SYSV_ABI lookup_async(int, const void*) {
    std::lock_guard<std::mutex> lock(g_lookup_mu);
    return g_lookup_req++;
}
int PS4_SYSV_ABI lookup_delete_req(int) {
    return 0;
}
int PS4_SYSV_ABI lookup_abort(int) {
    return 0;
}
int PS4_SYSV_ABI lookup_poll(int, int* result) {
    if (result) *result = 0;
    return 0;
}
int PS4_SYSV_ABI lookup_npid(int, const void* online, void* npid, void*) {
    if (!online || !npid) return kNpInvalidArg;
    NpFillNpId(npid, static_cast<const char*>(online));
    return 0;
}

// ---- NpScore: no leaderboard server. Contexts and requests are real handles; the
// synchronous queries report the community server as unreachable.

constexpr int kScoreErrOffline = static_cast<int>(0x80550605u);
std::atomic<int> g_score_req{1};
int PS4_SYSV_ABI score_title_ctx(const void*, int) {
    static std::atomic<int> id{1};
    return id.fetch_add(1);
}
int PS4_SYSV_ABI score_delete_title_ctx(int) {
    return 0;
}
int PS4_SYSV_ABI score_create_request(int) {
    return g_score_req.fetch_add(1);
}
int PS4_SYSV_ABI score_delete_request(int) {
    return 0;
}
int PS4_SYSV_ABI score_abort_request(int) {
    return 0;
}
int PS4_SYSV_ABI score_set_pc_id(int, int) {
    return 0;
}
int PS4_SYSV_ABI score_offline() {
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 4) Log("sceNpScore* query (no leaderboard server)");
    return kScoreErrOffline;
}
int PS4_SYSV_ABI score_comment_ok(int, const char*) {
    return 0;
}

} // namespace

void NpFillNpId(void* out, const char* online) {
    if (!out) return;
    std::memset(out, 0, 36);
    if (!online || !online[0]) online = "Player";
    std::snprintf(static_cast<char*>(out), 17, "%s", online);
}

std::string NpIdText(const void* npid) {
    if (!npid) return "";
    char buf[17]{};
    std::memcpy(buf, npid, 16);
    return buf;
}

void RegisterNp(Exports& t) {
    HD_EXPORT(t, "sceNpSetContentRestriction", np_restriction);
    HD_EXPORT(t, "sceNpSetNpTitleId", np_title);
    HD_EXPORT(t, "sceNpRegisterStateCallback", np_reg_state);
    HD_EXPORT(t, "sceNpUnregisterStateCallback", np_unreg_state);
    HD_EXPORT(t, "sceNpRegisterGamePresenceCallback", np_reg_presence);
    HD_EXPORT(t, "sceNpGetState", np_get_state);
    HD_EXPORT(t, "sceNpCheckCallback", np_check_cb_noop);
    HD_EXPORT(t, "sceNpCreateAsyncRequest", np_create_async_request);
    HD_EXPORT(t, "sceNpDeleteRequest", np_delete_request);
    HD_EXPORT(t, "sceNpAbortRequest", np_abort_request);
    HD_EXPORT(t, "sceNpPollAsync", np_poll_async);
    HD_EXPORT(t, "sceNpCheckNpAvailability", np_check_availability);
    HD_EXPORT(t, "sceNpGetParentalControlInfo", np_parental);
    HD_EXPORT(t, "sceNpCheckPlus", np_check_plus);
    HD_EXPORT(t, "sceNpRegisterPlusEventCallback", np_plus_cb);
    HD_EXPORT(t, "sceNpUnregisterPlusEventCallback", np_plus_cb);
    HD_EXPORT(t, "sceNpNotifyPlusFeature", np_plus_notify);
    HD_EXPORT(t, "sceNpAuthCreateAsyncRequest", npauth_create_async_request);
    HD_EXPORT(t, "sceNpAuthDeleteRequest", npauth_delete_request);
    HD_EXPORT(t, "sceNpAuthPollAsync", npauth_poll_async);
    HD_EXPORT(t, "sceNpAuthGetAuthorizationCode", npauth_get_code);
    HD_EXPORT(t, "sceNpGetOnlineId", np_online_id);
    HD_EXPORT(t, "sceNpGetNpId", np_get_npid);
    HD_EXPORT(t, "sceNpGetGamePresenceStatus", np_presence_status);
    HD_EXPORT(t, "sceNpCmpOnlineId", np_cmp_online);
    HD_EXPORT(t, "sceNpCmpNpId", np_cmp_npid);
    HD_EXPORT(t, "sceNpScoreCreateNpTitleCtx", score_title_ctx);
    HD_EXPORT(t, "sceNpWebApiInitialize", webapi_init);
    HD_EXPORT(t, "sceNpWebApiTerminate", webapi_term);
    HD_EXPORT(t, "sceNpWebApiCreateContext", webapi_create_ctx);
    HD_EXPORT(t, "sceNpWebApiDeleteContext", webapi_delete_ctx);
    HD_EXPORT(t, "sceNpWebApiCreatePushEventFilter", webapi_create_filter);
    HD_EXPORT(t, "sceNpWebApiDeletePushEventFilter", webapi_delete_filter);
    HD_EXPORT(t, "sceNpWebApiRegisterPushEventCallback", webapi_reg_push);
    HD_EXPORT(t, "sceNpWebApiUnregisterPushEventCallback", webapi_unreg_push);
    HD_EXPORT(t, "sceNpWebApiCreateRequest", webapi_create_req);
    HD_EXPORT(t, "sceNpWebApiDeleteRequest", webapi_delete_req);
    HD_EXPORT(t, "sceNpWebApiAbortRequest", webapi_abort_req);
    HD_EXPORT(t, "sceNpWebApiSendRequest", webapi_send);
    HD_EXPORT(t, "sceNpWebApiGetHttpStatusCode", webapi_status);
    HD_EXPORT(t, "sceNpWebApiReadData", webapi_read);
    HD_EXPORT(t, "sceNpWebApiGetHttpResponseHeaderValueLength", webapi_hdr_len);
    HD_EXPORT(t, "sceNpWebApiGetHttpResponseHeaderValue", webapi_hdr_val);
    HD_EXPORT(t, "sceNpWebApiUtilityParseNpId", webapi_parse_npid);
    HD_EXPORT(t, "sceNpLookupCreateTitleCtx", lookup_title_ctx);
    HD_EXPORT(t, "sceNpLookupDeleteTitleCtx", lookup_delete_ctx);
    HD_EXPORT(t, "sceNpLookupCreateAsyncRequest", lookup_async);
    HD_EXPORT(t, "sceNpLookupDeleteRequest", lookup_delete_req);
    HD_EXPORT(t, "sceNpLookupAbortRequest", lookup_abort);
    HD_EXPORT(t, "sceNpLookupPollAsync", lookup_poll);
    HD_EXPORT(t, "sceNpLookupNpId", lookup_npid);
    HD_EXPORT(t, "sceNpScoreDeleteNpTitleCtx", score_delete_title_ctx);
    HD_EXPORT(t, "sceNpScoreCreateRequest", score_create_request);
    HD_EXPORT(t, "sceNpScoreDeleteRequest", score_delete_request);
    HD_EXPORT(t, "sceNpScoreAbortRequest", score_abort_request);
    HD_EXPORT(t, "sceNpScoreSetPlayerCharacterId", score_set_pc_id);
    HD_EXPORT(t, "sceNpScoreGetRankingByNpIdPcId", score_offline);
    HD_EXPORT(t, "sceNpScoreGetRankingByRange", score_offline);
    HD_EXPORT(t, "sceNpScoreGetGameData", score_offline);
    HD_EXPORT(t, "sceNpScoreRecordScore", score_offline);
    HD_EXPORT(t, "sceNpScoreGetBoardInfo", score_offline);
    HD_EXPORT(t, "sceNpScoreRecordGameData", score_offline);
    HD_EXPORT(t, "sceNpScoreCensorComment", score_comment_ok);
    HD_EXPORT(t, "sceNpScoreSanitizeComment", score_comment_ok);
}

} // namespace Hd
