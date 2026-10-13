"""The Hunter's Dream co-op backend's Python side: the launcher's account calls (bbport_hd.py)
against a stand-in for the private server's /auth endpoints, and run.py's host routing and
ruleset for it. No network beyond 127.0.0.1."""
from paths import ROOT
import http.server
import importlib.util
import json
import os
from pathlib import Path
import ssl
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(ROOT / 'launcher'))
import bbport_hd  # noqa: E402


def load_run():
    spec = importlib.util.spec_from_file_location('bbport_run_under_test', ROOT / 'run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AuthServer:
    """The account service as bbhost's client sees it (its src/net/account.cpp): every reply
    carries ResKind, a refusal carries Message, and a limit is a 429 with both."""

    def __init__(self, tls=None):
        self.requests = []
        self.polls = 0
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
                owner.requests.append((self.path, body, dict(self.headers)))
                status, reply = owner.answer(self.path, body)
                data = reply if isinstance(reply, bytes) else json.dumps(reply).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.base = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        if tls:  # an ssl.SSLContext: the server speaks https, as the real one does
            self.httpd.socket = tls.wrap_socket(self.httpd.socket, server_side=True)
            self.base = self.base.replace('http://', 'https://')
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def answer(self, path, body):
        if path == '/auth/device/start':
            return 200, {'ResKind': 0, 'DeviceCode': 'secret-device', 'UserCode': 'ABCD-EFGH',
                         'VerificationUri': self.base + '/link', 'Interval': 2, 'ExpiresIn': 300}
        if path == '/auth/device/poll':
            self.polls += 1
            if body.get('DeviceCode') != 'secret-device':
                return 200, {'ResKind': 1, 'Message': 'unknown code'}
            if self.polls < 2:
                return 200, {'ResKind': 0, 'Status': 'pending'}
            return 200, {'ResKind': 0, 'Status': 'approved', 'OnlineId': 'Gehrman', 'Token': 'tok-1'}
        if path == '/auth/account/create':
            if body.get('Name') == 'Taken':
                return 409, {'ResKind': 1, 'Message': 'that name is taken'}
            if body.get('Name') == 'Flood':
                return 429, {'ResKind': 1, 'Message': 'too many accounts from this address'}
            return 200, {'ResKind': 0, 'OnlineId': body['Name'], 'Token': 'tok-2', 'RecoveryCode': 'RC-1'}
        if path == '/auth/recover':
            if body.get('RecoveryCode') != 'RC-1':
                return 403, {'ResKind': 1, 'Message': 'wrong recovery code'}
            return 200, {'ResKind': 0, 'OnlineId': body['Name'], 'Token': 'tok-3', 'RecoveryCode': 'RC-2'}
        if path == '/auth/web_code':
            return 200, {'ResKind': 0, 'Code': 'WEB-9'}
        if path == '/auth/logout':
            return 200, {'ResKind': 0}
        if path == '/auth/broken':
            return 200, b'<html>not json</html>'
        return 404, {'detail': 'Not Found'}


class AccountTests(unittest.TestCase):
    def setUp(self):
        self.server = AuthServer()
        self.addCleanup(self.server.close)
        self.base = self.server.base

    def test_device_link(self):
        link = bbport_hd.link_start(self.base)
        self.assertEqual((link['user_code'], link['interval'], link['expires_in']), ('ABCD-EFGH', 2, 300))
        path, body, headers = self.server.requests[-1]
        self.assertEqual(path, '/auth/device/start')
        self.assertTrue(body['DeviceName'].startswith('Bloodborne PC'))
        self.assertIn('bloodborne_pc', headers['User-Agent'])   # says who it is, not "bbhost"
        self.assertNotIn('bbhost/', headers['User-Agent'])
        self.assertEqual(bbport_hd.link_poll(self.base, link), ('pending', None))
        self.assertEqual(bbport_hd.link_poll(self.base, link),
                         ('approved', {'name': 'Gehrman', 'token': 'tok-1'}))
        self.assertEqual(self.server.requests[-1][1], {'DeviceCode': 'secret-device'})

    def test_create_recover_web_code_logout(self):
        account, code = bbport_hd.create(self.base, 'Eileen')
        self.assertEqual((account, code), ({'name': 'Eileen', 'token': 'tok-2'}, 'RC-1'))
        account, new_code = bbport_hd.recover(self.base, 'Eileen', 'RC-1')
        self.assertEqual((account, new_code), ({'name': 'Eileen', 'token': 'tok-3'}, 'RC-2'))
        self.assertEqual(bbport_hd.web_code(self.base, 'tok-3'), 'WEB-9')
        self.assertEqual(self.server.requests[-1][1], {'Token': 'tok-3'})
        bbport_hd.logout(self.base, 'tok-3')
        self.assertEqual(self.server.requests[-1][0], '/auth/logout')
        bbport_hd.logout(self.base, '')  # nobody signed in: no call
        self.assertEqual(sum(1 for r in self.server.requests if r[0] == '/auth/logout'), 1)

    def test_the_servers_message_is_what_the_player_reads(self):
        for name, message in (('Taken', 'that name is taken'),
                              ('Flood', 'too many accounts from this address')):  # a 429 has it too
            with self.assertRaises(bbport_hd.AccountError) as raised:
                bbport_hd.create(self.base, name)
            self.assertEqual(str(raised.exception), message)
        with self.assertRaises(bbport_hd.AccountError) as raised:
            bbport_hd.recover(self.base, 'Eileen', 'wrong')
        self.assertEqual(str(raised.exception), 'wrong recovery code')
        with self.assertRaises(bbport_hd.AccountError):
            bbport_hd.web_code(self.base, '')

    def test_a_server_without_the_account_service(self):
        with self.assertRaises(bbport_hd.AccountError) as raised:
            bbport_hd.auth_call(self.base, '/auth/unknown', {})
        self.assertIn('no account service (HTTP 404)', str(raised.exception))
        with self.assertRaises(bbport_hd.AccountError) as raised:
            bbport_hd.auth_call(self.base, '/auth/broken', {})
        self.assertEqual(str(raised.exception), 'bad reply')
        self.server.close()
        with self.assertRaises(bbport_hd.AccountError):
            bbport_hd.auth_call(self.base, '/auth/device/start', {}, timeout=2)

    def test_a_reply_without_a_token_signs_nobody_in(self):
        with self.assertRaises(bbport_hd.AccountError):
            bbport_hd.take_token({'ResKind': 0, 'OnlineId': 'Eileen'})
        with self.assertRaises(bbport_hd.AccountError):
            bbport_hd.take_token({'ResKind': 0, 'Token': 'tok'})


class TrustTests(unittest.TestCase):
    """The launcher's HTTPS trust (bbport_hd.context_from) against tests/hd_tls: a root, an
    intermediate that exists twice with one key and name - an expired certificate and its valid
    replacement - and a server certificate. The server sends its certificate with the valid
    intermediate, as thehuntersdream.com does with Let's Encrypt's reissued cross-signed roots."""
    TLS = ROOT / 'tests/hd_tls'

    @classmethod
    def entry(cls, name, trust=True):
        """A certificate as ssl.enum_certificates yields it from a Windows store."""
        return ssl.PEM_cert_to_DER_cert((cls.TLS / f'{name}.pem').read_text()), 'x509_asn', trust

    def setUp(self):
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(self.TLS / 'server_chain.pem', self.TLS / 'leaf.key')
        self.server = AuthServer(tls=tls)
        self.addCleanup(self.server.close)
        self.addCleanup(setattr, bbport_hd, '_trust', bbport_hd._trust)

    def link(self, context):
        bbport_hd._trust = context
        return bbport_hd.link_start(self.server.base)['user_code']

    def test_an_expired_copy_in_the_store_does_not_hide_the_servers_valid_one(self):
        store = [self.entry('root'), self.entry('mid_expired')]
        # What Python does by itself with such a store, and what a player then read.
        as_python_loads_it = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        for der, _encoding, _trust in store:
            as_python_loads_it.load_verify_locations(cadata=der)
        with self.assertRaises(bbport_hd.AccountError) as raised:
            self.link(as_python_loads_it)
        self.assertIn('certificate has expired', str(raised.exception))
        self.assertIn("Check the PC's date and time", str(raised.exception))
        # The launcher's own context leaves the expired copy out: the server's chain is fine.
        self.assertEqual(self.link(bbport_hd.context_from(store)), 'ABCD-EFGH')
        self.assertEqual(self.link(bbport_hd.context_from(store + [self.entry('mid_valid')])), 'ABCD-EFGH')

    def test_what_the_pc_does_not_trust_is_still_refused(self):
        for store in ([],                                         # nothing trusted
                      [self.entry('mid_expired')],                # only an expired authority
                      [self.entry('root', trust=frozenset())],    # trusted, but not for servers
                      [self.entry('root', trust=frozenset({'1.3.6.1.5.5.7.3.3'}))]):  # code signing
            with self.assertRaises(bbport_hd.AccountError) as raised:
                self.link(bbport_hd.context_from(store))
            self.assertIn("did not accept the server's certificate", str(raised.exception))
        # A root that is not valid yet (the PC's clock far in the past) is no root either.
        with self.assertRaises(bbport_hd.AccountError):
            self.link(bbport_hd.context_from([self.entry('root')], now=0))
        self.assertEqual(self.link(bbport_hd.context_from(
            [self.entry('root', trust=frozenset({bbport_hd.SERVER_AUTH}))])), 'ABCD-EFGH')

    def test_the_name_is_checked(self):
        # The certificate is for 127.0.0.1 and localhost, not for this name of the same server.
        context = bbport_hd.context_from([self.entry('root')])
        bbport_hd._trust = context
        base = self.server.base.replace('127.0.0.1', 'localhost.')
        with self.assertRaises(bbport_hd.AccountError):
            bbport_hd.link_start(base)

    @unittest.skipUnless(sys.platform == 'win32', 'the Windows certificate stores')
    def test_this_pcs_stores(self):
        bbport_hd._trust = None
        context = bbport_hd.trust_context()
        self.assertIs(bbport_hd.trust_context(), context)   # built once
        now = time.time()
        loaded = context.get_ca_certs()
        self.assertTrue(loaded)                              # this PC trusts something
        self.assertFalse([c['subject'] for c in loaded
                          if ssl.cert_time_to_seconds(c['notAfter']) < now])  # nothing expired
        # ... and it does not trust this test's own root.
        with self.assertRaises(bbport_hd.AccountError):
            bbport_hd.link_start(self.server.base)


class LinkPageTests(unittest.TestCase):
    def test_only_the_account_servers_own_pages_are_opened(self):
        base = 'https://thehuntersdream.com'
        ok = bbport_hd.safe_link_page
        self.assertEqual(ok('https://thehuntersdream.com/link', base), 'https://thehuntersdream.com/link')
        self.assertEqual(ok('https://www.thehuntersdream.com/link?c=1', base),
                         'https://www.thehuntersdream.com/link?c=1')
        self.assertEqual(ok('https://thehuntersdream.com/link', 'https://dev.thehuntersdream.com'),
                         'https://thehuntersdream.com/link')
        for bad in ('file:///C:/Windows/system32/calc.exe', 'javascript:alert(1)', '',
                    'https://evil.example/link', 'https://thehuntersdream.com.evil.example/link',
                    'http://thehuntersdream.com/link',   # a plain-http page of an https server
                    'https://evilthehuntersdream.com/link'):
            self.assertEqual(ok(bad, base), '', bad)
        self.assertEqual(ok('http://192.168.1.20:18671/link', 'http://192.168.1.20:18671'),
                         'http://192.168.1.20:18671/link')   # a server on the local network

    def test_auth_server(self):
        self.assertEqual(bbport_hd.auth_server('https://thehuntersdream.com/'), 'https://thehuntersdream.com')
        self.assertEqual(bbport_hd.auth_server('', ''), 'https://thehuntersdream.com')
        self.assertEqual(bbport_hd.auth_server('https://a', 'http://10.0.0.2:18671/'), 'http://10.0.0.2:18671')
        self.assertEqual(bbport_hd.auth_server('example.org'), 'https://example.org')

    def test_game_environment(self):
        env = bbport_hd.game_environment({'BB_HD_ACCOUNT_TOKEN': 'stale'}, 'https://thehuntersdream.com',
                                         'Eileen', 'tok')
        self.assertEqual(env, {'BB_ONLINE_BACKEND': 'hd', 'BB_COMMUNITY_SERVER': 'https://thehuntersdream.com',
                               'BB_HD_ACCOUNT_NAME': 'Eileen', 'BB_HD_ACCOUNT_TOKEN': 'tok'})
        env = bbport_hd.game_environment(env, 'https://thehuntersdream.com', 'Eileen', '')
        self.assertNotIn('BB_HD_ACCOUNT_TOKEN', env)   # half an account is none
        self.assertNotIn('BB_HD_ACCOUNT_NAME', env)


class RunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.script = load_run()

    def test_ruleset(self):
        rules = self.script.online_ruleset
        self.assertEqual(rules({}, False, False), 'vanilla')
        # Effects, the intro and the camera's distance change nothing for other players.
        self.assertEqual(rules({'effect_ssr': '1', 'skip_intro': '1', 'effect_dof': '0',
                                'tweak_camera_distance': '1', 'tweak_no_camera_rotation': '1',
                                'cheat_no_death': '0'}, False, False), 'vanilla')
        self.assertEqual(rules({'cheat_no_death': '1'}, False, False), 'bbpc-cheats')
        self.assertEqual(rules({'tweak_no_rally': '1'}, False, False), 'bbpc-tweaks')
        self.assertEqual(rules({'debug_menu': '1'}, False, False), 'bbpc-debug')
        self.assertEqual(rules({}, True, False), 'bbpc-mods')
        self.assertEqual(rules({}, False, True), 'bbpc-patches')
        everything = rules({'cheat_stealth': '1', 'tweak_ragdoll': '1', 'debug_camera': '1'}, True, True)
        self.assertEqual(everything, 'bbpc-cheats,bbpc-debug,bbpc-mods,bbpc-patches,bbpc-tweaks')
        # What the server and the game accept as a ruleset (hd_config.cpp CleanRuleset).
        self.assertRegex(everything, r'^[a-z0-9_.;=,-]{1,256}$')

    def test_the_old_hunters_patch_is_no_change_of_rules(self):
        # bbhost ships this write as its own patch, on by default, and stays "vanilla".
        import xml.etree.ElementTree as ET
        changes = self.script.changes_rules

        def patch(*lines):
            return ET.fromstring('<Metadata Name="x" AppVer="01.09"><PatchList>%s</PatchList></Metadata>' % ''.join(
                '<Line Type="%s" Address="%s" Value="%s"/>' % line for line in lines))
        old_hunters = ('bytes', '0x023b6d20', 'b801000000')
        self.assertFalse(changes(patch(old_hunters)))
        self.assertFalse(changes(patch(('bytes', '0x23B6D20', 'B8 01 00 00 00'))))  # as files write it
        self.assertTrue(changes(patch(old_hunters, ('bytes', '0x01000000', '90'))))  # and something else
        self.assertTrue(changes(patch(('bytes', '0x023b6d20', 'b800000000'))))       # another value there
        self.assertTrue(changes(patch(('bytes', '0x01000000', '90'))))
        self.assertTrue(changes(patch(('nonsense', '0x023b6d20', 'b801000000'))))    # not understood: counts
        self.assertTrue(changes(patch()))

    def test_every_cheat_and_tweak_of_the_launcher_is_classified(self):
        from patches import EFFECTS
        for key, (_off, on) in EFFECTS.items():
            if not on or not key.startswith(('cheat_', 'tweak_')):
                continue
            expected = 'vanilla' if key in self.script.COSMETIC_TWEAKS else \
                'bbpc-cheats' if key.startswith('cheat_') else 'bbpc-tweaks'
            self.assertEqual(self.script.online_ruleset({key: '1'}, False, False), expected, key)

    def overrides(self, env, summon=None):
        with tempfile.TemporaryDirectory() as directory:
            probe = mock.Mock(side_effect=AssertionError('no shadNet probe for this backend')) \
                if summon is None else mock.Mock(return_value=summon)
            with mock.patch.object(self.script, 'summon_service', probe), \
                    mock.patch('builtins.print'):
                self.script.online_overrides(env, Path(directory))
            self.assertEqual(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'],
                             str(Path(directory) / 'host_overrides.json'))
            return json.loads((Path(directory) / 'host_overrides.json').read_text(encoding='utf-8'))

    def test_hunters_dream_routing(self):
        routes = self.overrides({'BB_ONLINE_BACKEND': 'hd', 'BB_COMMUNITY_SERVER': 'https://thehuntersdream.com'})
        self.assertEqual(routes, {
            'https://ss4.scej-network.jp:20443': 'https://thehuntersdream.com',
            'bb-playlog-prod.s3.amazonaws.com': 'https://thehuntersdream.com:18671',
            'bb-playlog-test.s3.amazonaws.com': 'https://thehuntersdream.com:18671'})
        # Summon signs stay with the community server: nothing sends them to a shadNet WebAPI.
        self.assertFalse(any('summon_messenger' in key for key in routes))

    def test_shadnet_routing_is_as_before(self):
        env = {'BB_SHADNET_WEBAPI': 'http://10.0.0.5:31315'}
        self.assertEqual(self.overrides(dict(env), summon=True), {
            'https://ss4.scej-network.jp:20443': 'https://thehuntersdream.com',
            'https://thehuntersdream.com:18671/summon_messenger': 'http://10.0.0.5:31315',
            'http://thehuntersdream.com:18671/summon_messenger': 'http://10.0.0.5:31315'})
        self.assertEqual(self.overrides(dict(env), summon=False),
                         {'https://ss4.scej-network.jp:20443': 'https://thehuntersdream.com'})

    def test_a_custom_file_is_used_as_it_is(self):
        with tempfile.TemporaryDirectory() as directory:
            custom = Path(directory) / 'host_overrides.custom.json'
            custom.write_text('{"*": "http://127.0.0.1:1"}', encoding='utf-8')
            env = {'BB_ONLINE_BACKEND': 'hd'}
            self.script.online_overrides(env, Path(directory))
            self.assertEqual(env['SHADPS4_HTTP_HOST_OVERRIDES_JSON'], str(custom))
            self.assertFalse((Path(directory) / 'host_overrides.json').exists())


@unittest.skipUnless(sys.platform == 'win32', 'the Windows launcher')
class LauncherEnvironmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('bbport_launcher_under_test',
                                                      ROOT / 'launcher/bbport_launcher_win.py')
        cls.launcher = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.launcher)

    def settings(self, **changes):
        return {**self.launcher.APP_DEFAULTS, 'online': True, 'online_npid': 'shadnet-name',
                'online_password': 'shadnet-secret', **changes}

    def test_hunters_dream_backend(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            for name in [n for n in os.environ if n.startswith(('BB_HD_', 'BB_SHADNET_', 'BB_ONLINE'))]:
                del os.environ[name]
            env = self.launcher.game_environment(self.settings(
                online_backend='hd', hd_account_name='Eileen', hd_account_token='tok'))
        self.assertEqual(env['BB_ONLINE'], '1')
        self.assertEqual(env['BB_ONLINE_BACKEND'], 'hd')
        self.assertEqual(env['BB_COMMUNITY_SERVER'], 'https://thehuntersdream.com')
        self.assertEqual((env['BB_HD_ACCOUNT_NAME'], env['BB_HD_ACCOUNT_TOKEN']), ('Eileen', 'tok'))
        # The shadNet account is not handed to a game that does not sign in there.
        self.assertNotIn('BB_SHADNET_PASSWORD', env)
        self.assertNotIn('BB_SHADNET_NPID', env)

    def test_shadnet_backend_is_as_before(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            for name in [n for n in os.environ if n.startswith(('BB_HD_', 'BB_SHADNET_', 'BB_ONLINE'))]:
                del os.environ[name]
            env = self.launcher.game_environment(self.settings(hd_account_name='Eileen', hd_account_token='tok'))
            offline = self.launcher.game_environment(self.settings(online=False, online_backend='hd'))
        self.assertEqual(env['BB_ONLINE'], '1')
        self.assertNotIn('BB_ONLINE_BACKEND', env)
        self.assertEqual(env['BB_SHADNET_NPID'], 'shadnet-name')
        self.assertEqual(env['BB_SHADNET_SERVER'], 'srv.shadps4.net:31313')
        self.assertNotIn('BB_HD_ACCOUNT_TOKEN', env)   # nor the other way round
        self.assertNotIn('BB_ONLINE', offline)
        self.assertNotIn('BB_ONLINE_BACKEND', offline)


if __name__ == '__main__':
    unittest.main()
