# Co-op through The Hunter's Dream (bbhost's online)

A second co-op service beside shadNet: the online implementation of
[bbhost](https://github.com/droogie/bbhost) (droogie and contributors, GPL-3.0-or-later),
ported into this port. With it the game's PSN calls go to The Hunter's Dream's own session
service, so players of this port and bbhost players can meet in the same rooms.

**Status: experimental.** It builds into `bb-probe.exe` and passes its tests: two copies of
the backend summon, join, exchange datagrams, kick and leave through a stand-in for the
server, directly and through the relay (see Testing). It has not yet been played in the game
against the public server: the stand-in is the server as bbhost's client describes it, not
the server itself. Please report how it goes with `user\last_run.log` (every line of this
backend starts with `Co-op:`).

The two services do not meet: everyone in a session picks the same one.

| | shadNet (default) | The Hunter's Dream |
|---|---|---|
| Messages, bloodstains, ghosts | The Hunter's Dream | The Hunter's Dream |
| Bells and summons | a shadNet server | The Hunter's Dream's session service |
| Account | shadNet name and password | an account linked to this PC, no password |
| Players you meet | shadPS4 and this port | bbhost and this port |
| NAT | UPnP | STUN, hole punching, the server's relay |

## Setting it up

1. Launcher, **Online co-op**: tick **Play online** and choose **Play with: The Hunter's
   Dream (bbhost players)**.
2. Under **The Hunter's Dream account**, link this PC:
   - **Link with Discord…** shows a code; approve it on the website, signed in with Discord.
   - **New account on this PC…** makes an account held by this PC alone and shows its
     recovery code once. Write it down: it is the only way back into the account.
   - **Recover…** signs a new PC in with the name and the recovery code (every other PC of
     the account is signed out, and a new code is shown).
3. **Test connection**, then **PLAY** and choose online at the title screen.

The public server plays only with signed-in players: without a linked account the game
starts offline. The link (a token) is saved with the other launcher settings and sent only to
the game server.

## Fair play

The server keeps modified sessions apart from unmodified ones. This port tells it which kind
a session is (`X-BBHost-Ruleset`, computed at every start in `run.py`):

- `vanilla`: no cheats, no gameplay tweaks, no debug menu or free camera, no mods, no extra
  patches. Effects, the frame rate, the intro skip and the two camera tweaks do not count.
  Nor does the Old Hunters patch (one write at `0x023b6d20`) as an extra patch: bbhost ships
  the same write as its own `old-hunters` patch, on by default, and announces nothing for it.
- otherwise the kinds that are on: `bbpc-cheats`, `bbpc-tweaks`, `bbpc-debug`, `bbpc-mods`,
  `bbpc-patches`.

Cheating against other players gets an account banned on The Hunter's Dream.

## What is ported, and from where

Everything is in `gpu/bbnet/hd/` (GPL-3.0-or-later), each file a port of the bbhost source
named at its top:

| Here | bbhost | What |
|---|---|---|
| `hd_session.cpp` | `src/net/session.cpp` | the dispatcher for the game's callbacks, the event poller, the peer table, the keepalive, the `/mp/matching2/*` calls |
| `hd_np_matching2.cpp` | `src/hle/np_matching2.cpp` | sceNpMatching2: rooms, members, kicks, the signaling events, heartbeats |
| `hd_np_signaling.cpp` | `src/hle/np_signaling.cpp` | sceNpSignaling: connections to a peer, resolved by the server before the peer joins |
| `hd_np.cpp` | `src/hle/system.cpp` (NP part) | signed in as the account, PlayStation Plus, the authorization code, empty friend lists |
| `hd_p2p.cpp` | `src/hle/net.cpp` (P2P part) | the P2P UDP port: the PS4 vport header, STUN on the game's own port, hole-punch probes, the relay's frames |
| `hd_stun.cpp` | `src/net/stun.cpp` | the STUN Binding exchange and the relay's HELLO |
| `hd_http.cpp`, `hd_account.cpp` | `src/net/http.cpp`, `account.cpp` | the backend's own requests, the `X-BB-Token` and `X-BBHost-Ruleset` headers |
| `hd_engine.cpp` | `src/engine/summon_invite.cpp`, `sos_signs.cpp`, `playlog.cpp`, `chara_id.cpp` | 1.09 only: the signaling gate, the host's area for an invite, a 30 s sign timeout, the play log every 5 s, the character id fix |
| `launcher/bbport_hd.py` | `src/net/account.cpp` | linking, creating and recovering an account |

The sockets and sceHttp stay this port's (shadPS4's); `p2p_sockets.cpp` hands the P2P port to
`hd_p2p.cpp`, `http.cpp` adds the two headers to the game's requests to the server, and
`src/runtime_services.c` asks the backend first for every `sceNp*` import.

Not ported:

- **Five players** (two invaders in the late areas): bbhost rewrites the maps' event scripts
  for it. A host on this port keeps the game's own limits; joining a bbhost host works as
  the game allows.
- **Playing by a modded host's rules** (bbhost's randomizer and boss rush plugins): a guest
  from this port plays the host's world with its own files.
- **The in-game account screen** (bbhost's F10): the launcher links the account.
- bbhost's own experimental invite delivery (off by default there).

The character id fix runs when the game signs in and before each request to the server,
where bbhost hooks the login step itself: a character holding the "no id" marker is fixed at
the next request rather than at exactly that step.

## Settings

The launcher sets these; `Advanced → extra environment` can add the rest.

| Variable | Default | |
|---|---|---|
| `BB_ONLINE_BACKEND` | (shadNet) | `hd` chooses this service |
| `BB_COMMUNITY_SERVER` | `https://thehuntersdream.com` | the server: the game's API and the session service on port 18671 |
| `BB_HD_NP_SERVER` | | the session service's base address, for a server of your own (`http://192.168.1.20:18671`) |
| `BB_HD_AUTH_SERVER` | the server | where the account calls go |
| `BB_HD_VERIFY_TLS` | `1` | `0` for a self-signed server on your network |
| `BB_HD_REQUIRE_ACCOUNT` | `1` | `0` plays as `BB_HD_ONLINE_ID` without an account (a server of your own) |
| `BB_HD_P2P_PORT` | `9307` | this game's UDP port; the next free one is taken when it is in use |
| `BB_HD_STUN_SERVER` | the server, port 3478 | `host[:port]`, or `off` |
| `BB_HD_RELAY` | `1` | `0` never uses the server's relay |
| `BB_HD_SIGN_TIMEOUT` | `30` | seconds a summon sign may take to answer (the game's own: 180; `0` keeps it) |
| `BB_HD_ANSWER_SECONDS` | `15` | least time a session call waits for the server's answer (`0`: bbhost's 3 to 5 s, too short from far away) |
| `BB_HD_PLAYLOG_SECONDS` | `5` | how often the play log goes to the server (the game's own: 300; `0` keeps it) |
| `BB_HD_PLAYLOG_SAMPLE_MS` | `500` | how often the position is logged (the game's own: 1500; `0` keeps it) |
| `BB_HD_TRACE` | `0` | `1` logs every callback and datagram |

## Certificates

The launcher's account calls and the game's session calls check the server's certificate
against this PC's Windows stores (`ROOT` and `CA`), without the entries that are not valid
today. OpenSSL prefers a certificate from its trust store to the one the server sends, so a PC
that still holds an expired copy of an intermediate (Windows keeps the ones it has seen)
otherwise refuses a server whose own chain is valid: "certificate has expired". The same
message on a PC whose clock is wrong is true: check its date and time.

A failed session call is logged with the time it took (`np: POST /np/events/poll -> ...
(after 4012 ms)`): the call's whole timeout is a server that did not answer, a few
milliseconds is a connection that was refused or closed.

## Testing

None of these needs the game or reaches beyond 127.0.0.1.

- `cd tests && python -m unittest test_online_hd`: the launcher's account calls against a
  stand-in account server, the host routing and the ruleset, and the certificate check with
  an expired intermediate in the store (`tests/hd_tls/`).
- `ninja -C out/gpu hd-stun-test && out/gpu/hd-stun-test`: bbhost's own test of the STUN
  exchange.
- `ninja -C out/gpu hd-session-test`, then `cd tests && python -m unittest test_hd_session`:
  the whole backend without the game. `tests/test_hd_session.cpp` calls it through the game's
  own imports and reads the callbacks' structures at the offsets the game reads; a host and a
  guest meet through `tests/hd_mock_server.py` (the session service, the event queue, STUN
  and the relay as bbhost's client uses them): a summon with the invite over the P2P port,
  the join, datagrams both ways, heartbeats, leaving, a kick with its reason, the relay, and
  the account and ruleset headers. `HD_TEST_TRACE=1` prints every callback and datagram with
  its time.
