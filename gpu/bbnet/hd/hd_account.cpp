// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/net/account.cpp (the state; the linking calls are the launcher's,
// launcher/bbport_hd.py).
#include "hd/hd_account.h"

#include <atomic>

#include "hd/hd.h"

namespace Hd {

namespace {
std::atomic<bool> g_refused{false};
} // namespace

std::string AccountName() {
    const Config& c = Cfg();
    return c.account_token.empty() ? std::string() : c.account_name;
}

std::string AccountToken() {
    const Config& c = Cfg();
    return c.account_name.empty() ? std::string() : c.account_token;
}

bool AccountLoggedIn() {
    const Config& c = Cfg();
    return !c.account_token.empty() && !c.account_name.empty();
}

bool AccountRefused() {
    return g_refused.load();
}

void AccountMarkRefused(const std::string& reason) {
    if (g_refused.exchange(true)) return;
    const std::string name = AccountName();
    Log("account: the server refused this PC's sign-in%s%s (%s): until it is linked again "
        "(launcher, Online co-op) nobody can summon or invite this player",
        name.empty() ? "" : " as ", name.c_str(), reason.c_str());
}

} // namespace Hd
