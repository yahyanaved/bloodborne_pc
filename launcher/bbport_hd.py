# SPDX-License-Identifier: GPL-3.0-or-later
"""The Hunter's Dream account of the launcher: bbhost's account calls (its src/net/account.cpp,
github.com/droogie/bbhost, GPL-3.0-or-later) for the "Online co-op" page.

The private server has no passwords. A PC is linked to an account the player signs into on
the website with Discord (a device code, as TVs do it), or makes an account held by this PC
alone, with a one-time recovery code. Either way the server hands back a token; the launcher
keeps it with its settings and gives it to the game, which sends it with every request to the
server (the X-BB-Token header), so the account's name is the player's online name.

Every call here talks to the server (blocking: run them off the Tk thread) and raises
AccountError with the server's message when it fails. Tokens and recovery codes are never
printed or logged.
"""
import json
import os
import socket
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

PUBLIC_SERVER = 'https://thehuntersdream.com'
# This port says who it is: the server's operators tell clients apart by this.
USER_AGENT = 'bloodborne_pc launcher (bbhost online protocol)'
SERVER_AUTH = '1.3.6.1.5.5.7.3.1'  # the "TLS server" use of a certificate in a Windows store


class AccountError(Exception):
    """The account server refused or could not be reached; str() is what the player reads."""


def context_from(entries, now=None):
    """An SSL context that trusts the Windows store entries given ((DER, encoding, trust), as
    ssl.enum_certificates yields them) that are valid today.

    Python's own default copies the Windows stores into OpenSSL as they are, expired
    certificates included, and OpenSSL prefers a certificate it finds in its trust store to
    the one the server sends. A PC that still holds an old, expired copy of an intermediate
    (Windows keeps the ones it has seen; Let's Encrypt's cross-signed roots were reissued)
    then refuses a server whose own chain is valid: "certificate has expired". Without the
    expired entries OpenSSL takes the server's valid one."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)  # verifies the chain and the name
    # As ssl.create_default_context() sets them.
    context.verify_flags |= getattr(ssl, 'VERIFY_X509_PARTIAL_CHAIN', 0) | getattr(ssl, 'VERIFY_X509_STRICT', 0)
    now = time.time() if now is None else now
    for der, encoding, trust in entries:
        if encoding != 'x509_asn' or not (trust is True or SERVER_AUTH in trust):
            continue
        try:
            probe = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            probe.load_verify_locations(cadata=der)
            dates = probe.get_ca_certs()  # empty for a certificate that is not an authority's
            if dates and not (ssl.cert_time_to_seconds(dates[0]['notBefore']) <= now
                              <= ssl.cert_time_to_seconds(dates[0]['notAfter'])):
                continue
            context.load_verify_locations(cadata=der)
        except (ssl.SSLError, ValueError):
            continue
    return context


_trust = None


def trust_context():
    """The context of the launcher's HTTPS calls: what this PC trusts (context_from)."""
    global _trust
    if _trust is None:
        if sys.platform == 'win32':
            entries = []
            for store in ('ROOT', 'CA'):
                try:
                    entries += ssl.enum_certificates(store)
                except OSError:
                    pass
            _trust = context_from(entries)
            _trust.set_default_verify_paths()  # SSL_CERT_FILE, as the default context honours it
        else:
            _trust = ssl.create_default_context()
    return _trust


def failure_text(error):
    """What the player reads when a call did not get through."""
    reason = getattr(error, 'reason', None) or error
    if isinstance(reason, ssl.SSLCertVerificationError):
        return (f"this PC did not accept the server's certificate ({reason.verify_message}). "
                "Check the PC's date and time.")
    return str(reason)


def auth_server(community, override=''):
    """Where the account calls go: the override (BB_HD_AUTH_SERVER) or the game server's origin."""
    base = (override or community or PUBLIC_SERVER).strip().rstrip('/')
    if '://' not in base:
        base = 'https://' + base
    return base


def device_name():
    """What the website's /link page shows the player before they approve, so a code phished
    from another machine stands out."""
    host = os.environ.get('COMPUTERNAME') or ''
    if not host:
        try:
            host = socket.gethostname()
        except OSError:
            host = ''
    return f'Bloodborne PC on {host[:48]}' if host else 'Bloodborne PC'


def auth_call(base, path, body, timeout=8):
    """POST to the account server and read the reply whatever the status: a limit is a 429 with
    the reason in Message, which is what the player should see."""
    request = urllib.request.Request(
        base.rstrip('/') + path, data=json.dumps(body).encode('utf-8'), method='POST',
        headers={'Content-Type': 'application/json', 'Accept': 'application/json',
                 'User-Agent': USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=timeout, context=trust_context()) as reply:
            status, text = reply.status, reply.read(1 << 20)
    except urllib.error.HTTPError as error:
        with error:
            status, text = error.code, error.read(1 << 20)
    except (OSError, ValueError) as error:
        raise AccountError(failure_text(error)) from error
    try:
        data = json.loads(text or b'{}')
    except ValueError:
        raise AccountError('bad reply' if 200 <= status < 300 else f'HTTP {status}') from None
    # Every account reply carries ResKind. A server without the account service answers its
    # framework's own "not found" instead.
    if not isinstance(data, dict) or 'ResKind' not in data:
        raise AccountError(f'{base} has no account service (HTTP {status})')
    try:
        refused = int(data['ResKind']) != 0
    except (TypeError, ValueError):
        refused = True
    if refused:
        raise AccountError(str(data.get('Message') or 'refused'))
    return data


def take_token(reply):
    """The account a reply signs this PC into: {'name', 'token'}."""
    token, name = str(reply.get('Token') or ''), str(reply.get('OnlineId') or '')
    if not token or not name:
        raise AccountError('no token in the reply')
    return {'name': name, 'token': token}


def link_start(base):
    """Linking with the website: a code for the player to approve at 'verification_uri' (signed
    in with Discord). Poll every 'interval' seconds until the status is no longer "pending"."""
    reply = auth_call(base, '/auth/device/start', {'DeviceName': device_name()})
    link = {'device_code': str(reply.get('DeviceCode') or ''),      # secret, stays here
            'user_code': str(reply.get('UserCode') or ''),          # what the player types
            'verification_uri': str(reply.get('VerificationUri') or ''),
            'interval': 5, 'expires_in': 600}
    for key, field in (('interval', 'Interval'), ('expires_in', 'ExpiresIn')):
        try:
            link[key] = int(reply.get(field, link[key]))
        except (TypeError, ValueError):
            pass
    if link['interval'] < 1:
        link['interval'] = 5
    if not link['device_code'] or not link['user_code']:
        raise AccountError('no code in the reply')
    return link


def link_poll(base, link):
    """("pending" | "slow_down" | "approved" | "denied" | "expired", the account on "approved")."""
    reply = auth_call(base, '/auth/device/poll', {'DeviceCode': link['device_code']})
    status = str(reply.get('Status') or '')
    return status, take_token(reply) if status == 'approved' else None


def create(base, name):
    """An account held by this PC: (the account, a recovery code the player must write down -
    shown once, never saved)."""
    reply = auth_call(base, '/auth/account/create', {'Name': name, 'DeviceName': device_name()})
    return take_token(reply), str(reply.get('RecoveryCode') or '')


def recover(base, name, code):
    """A lost or signed-out PC: the name and its recovery code give (the account, a new recovery
    code); every other token of the account stops working."""
    reply = auth_call(base, '/auth/recover', {'Name': name, 'RecoveryCode': code})
    return take_token(reply), str(reply.get('RecoveryCode') or '')


def web_code(base, token):
    """A one-time code to sign in on the website with this PC's account."""
    if not token:
        raise AccountError('not signed in')
    code = str(auth_call(base, '/auth/web_code', {'Token': token}).get('Code') or '')
    if not code:
        raise AccountError('no code in the reply')
    return code


def logout(base, token):
    """Tells the server to forget the token; signing out here does not wait on it."""
    if not token:
        return
    try:
        auth_call(base, '/auth/logout', {'Token': token}, timeout=3)
    except AccountError:
        pass


def safe_link_page(uri, base):
    """The page to open in the browser for a link code, or '' when the reply's address is not a
    web page of the account server (a browser is not handed anything else)."""
    try:
        page, server = urllib.parse.urlsplit(uri), urllib.parse.urlsplit(base)
    except ValueError:
        return ''
    if page.scheme not in ('https', 'http') or not page.hostname or not server.hostname:
        return ''
    if page.scheme == 'http' and server.scheme == 'https':
        return ''
    host, home = page.hostname.lower(), server.hostname.lower()
    if host != home and not host.endswith('.' + home) and not home.endswith('.' + host):
        return ''
    return uri


def game_environment(env, community, account_name, account_token):
    """The game's settings for the Hunter's Dream backend (gpu/bbnet/hd/hd_config.cpp)."""
    env['BB_ONLINE_BACKEND'] = 'hd'
    env['BB_COMMUNITY_SERVER'] = community
    if account_name and account_token:
        env['BB_HD_ACCOUNT_NAME'] = account_name
        env['BB_HD_ACCOUNT_TOKEN'] = account_token
    else:
        env.pop('BB_HD_ACCOUNT_NAME', None)
        env.pop('BB_HD_ACCOUNT_TOKEN', None)
    return env


SESSION_PORT = 18671  # the private server's API port: the session service and the game's API


def server_check(community, timeout=6):
    """[(kind, address, error or None)] for the addresses the backend uses: 'game' is the game
    server's ss.info, 'server' the session service's port (a connection, nothing sent)."""
    host = urllib.parse.urlsplit(community).hostname or ''
    results = []
    try:
        with urllib.request.urlopen(f'{community}:20443/bb-eu/ss.info', timeout=timeout,
                                    context=trust_context()) as reply:
            reply.read(64)
        results.append(('game', community, None))
    except (OSError, ValueError) as error:
        results.append(('game', community, failure_text(error)))
    try:
        socket.create_connection((host, SESSION_PORT), timeout=timeout).close()
        results.append(('server', f'{host}:{SESSION_PORT}', None))
    except (OSError, ValueError) as error:
        results.append(('server', f'{host}:{SESSION_PORT}', str(error)))
    return results
