// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from bbhost: src/net/account.h.
//
// The player's account on the private server. There are no passwords: a PC is linked to an
// account the player signs into on the website with Discord (a device code, as TVs do it),
// or makes an account held by that PC alone, with a one-time recovery code. Either way the
// server hands back a token. With a token, Hd::OnlineId() is the account's name, and every
// request to the server carries the token in the X-BB-Token header (hd_http.cpp for the
// backend's own calls, core/libraries/network/http.cpp for the game's), so the server binds
// the traffic to the account whatever a settings file says.
//
// bbhost links the account on its in-game F10 screen; here the launcher does (its "Online
// co-op" page, launcher/bbport_hd.py) and hands the game the name and the token.
#pragma once

#include <string>

namespace Hd {

// Both empty when nobody is signed in. Safe from any thread.
std::string AccountName();
std::string AccountToken();
bool AccountLoggedIn();

// The server refused this PC's sign-in (HTTP 401 on its surfaces): the token expired or was
// replaced, or, with no token, the online id is an account's name. Kept for the session;
// the event poller slows down while it holds. AccountMarkRefused logs the reason once.
bool AccountRefused();
void AccountMarkRefused(const std::string& reason);

} // namespace Hd
