"""The Hunter's Dream co-op backend (gpu/bbnet/hd) without the game: a host and a guest - two
copies of out/gpu/hd-session-test, which calls the backend through the game's own imports -
meet through a stand-in for the private server (hd_mock_server.py) on 127.0.0.1.

Skipped until the program is built: ninja -C out/gpu hd-session-test."""
from paths import ROOT
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from hd_mock_server import MockServer, kick_reason

PROGRAM = next((path for path in (ROOT / 'out/gpu/hd-session-test.exe', ROOT / 'out/gpu/hd-session-test')
                if path.is_file()), None)


@unittest.skipUnless(PROGRAM, 'build it first: ninja -C out/gpu hd-session-test')
class SessionTests(unittest.TestCase):
    def play(self, relay=False, kick=False, accounts=None, hold_ms=0, ruleset='vanilla'):
        """(server, host output, guest output) of one summon."""
        tokens = {token: name for name, token in (accounts or {}).items()}
        server = MockServer(relay=relay, accounts=tokens)
        self.addCleanup(server.close)

        def start(role, name, peer, port, *flags):
            env = dict(os.environ)
            for stale in [key for key in env if key.startswith(('BB_HD_', 'BB_ONLINE', 'BB_COMMUNITY'))]:
                del env[stale]
            env.update(BB_ONLINE='1', BB_ONLINE_BACKEND='hd', BB_HD_NP_SERVER=server.base,
                       BB_HD_STUN_SERVER=f'127.0.0.1:{server.stun_port}', BB_HD_P2P_PORT=str(port),
                       BB_HD_RULESET=ruleset)
            if accounts and name in accounts:
                env.update(BB_HD_ACCOUNT_NAME=name, BB_HD_ACCOUNT_TOKEN=accounts[name])
            else:
                env.update(BB_HD_REQUIRE_ACCOUNT='0', BB_HD_ONLINE_ID=name)
            # A build in the MSYS2 tree needs its CLANG64 DLLs (as run.py arranges for bb-probe).
            clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
            if sys.platform == 'win32' and clang64.is_dir():
                env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
            # Into a file, not a pipe: both programs write while this test waits on one of them,
            # and a full pipe would stop the other in the middle of the session.
            log = tempfile.TemporaryFile(mode='w+', encoding='utf-8', errors='replace')
            self.addCleanup(log.close)
            process = subprocess.Popen([str(PROGRAM), role, peer, *flags], env=env, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT)
            return process, log

        flags = (['--kick'] if kick else []) + (['--hold-ms', str(hold_ms)] if hold_ms else [])
        # The guest is online first (its sign would be up); the host then summons it. Two ports
        # that are not the default, so a running game is not in the way.
        guest, guest_log = start('guest', 'GuestB', 'HostA', 19417, *flags)
        host, host_log = start('host', 'HostA', 'GuestB', 19407, *flags)
        outputs = []
        for process, log in ((host, host_log), (guest, guest_log)):
            timed_out = ''
            try:
                process.wait(timeout=120)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                timed_out = '\n(timed out)'
            log.seek(0)
            outputs.append(log.read() + timed_out)
        report = f'--- host ---\n{outputs[0]}\n--- guest ---\n{outputs[1]}'
        self.assertEqual((host.returncode, guest.returncode), (0, 0), report)
        for output in outputs:
            self.assertIn('RESULT ok', output, report)
            self.assertNotIn('FAIL', output, report)
        return server, outputs[0], outputs[1]

    def test_summon_join_traffic_and_leave(self):
        server, host, guest = self.play(hold_ms=6500)
        self.assertIn('host: GuestB joined as member 2', host)
        self.assertIn('datagrams both ways with GuestB', host)
        self.assertIn('datagrams both ways with HostA', guest)
        self.assertIn('host: member 2 left (cause 1)', host)
        # What the server was told, in bbhost's words.
        started = {body['OnlineId']: body for body, _headers in server.called('/mp/matching2/context_start')}
        self.assertEqual(set(started), {'HostA', 'GuestB'})
        self.assertEqual((started['HostA']['SignalingPort'], started['GuestB']['SignalingPort']), (19407, 19417))
        # No relay on this server: STUN reported each port as the world (here, loopback) sees it.
        self.assertEqual((started['HostA']['MappedAddr'], started['HostA']['MappedPort']), ('127.0.0.1', 19407))
        created, headers = server.called('/mp/matching2/create_room')[0]
        self.assertEqual((created['OnlineId'], created['MaxMembers'], created['LocalPort']), ('HostA', 5, 19407))
        self.assertEqual(headers.get('X-BBHost-Ruleset') or headers.get('X-Bbhost-Ruleset'), 'vanilla')
        self.assertIn('bloodborne_pc', headers['User-Agent'])
        self.assertNotIn('X-BB-Token', headers)   # nobody is signed in to an account here
        joined = server.called('/mp/matching2/join_room')[0][0]
        self.assertEqual((joined['OnlineId'], joined['RoomId']), ('GuestB', 1001))
        self.assertEqual([body['OnlineId'] for body, _h in server.called('/np/signaling/resolve')], ['GuestB'])
        self.assertTrue(server.called('/np/events/ack'))
        # The guest leaves, then the host: the room is gone, and it was kept alive meanwhile.
        self.assertEqual([body['MemberId'] for body, _h in server.called('/mp/matching2/leave_room')], [2, 1])
        self.assertEqual(server.rooms, {})
        self.assertGreaterEqual(server.heartbeats, 1)
        self.assertEqual(server.relayed, 0)

    def test_through_the_servers_relay(self):
        server, host, guest = self.play(relay=True)
        self.assertIn('datagrams both ways with GuestB', host)
        self.assertIn('datagrams both ways with HostA', guest)
        # Each client's address for its peers is its relay port, and every datagram went
        # through the relay: the invite before the join included.
        started = {body['OnlineId']: body for body, _headers in server.called('/mp/matching2/context_start')}
        ports = {entry['vport'] for entry in server.relay_by_token.values()}
        self.assertEqual(len(ports), 2)
        self.assertEqual({started['HostA']['MappedPort'], started['GuestB']['MappedPort']}, ports)
        self.assertGreaterEqual(server.relayed, 4)

    def test_kick(self):
        server, host, guest = self.play(kick=True)
        self.assertIn('host: member 2 left (cause 2)', host)
        self.assertIn("guest: kicked, with the host's reason", guest)
        kicked = server.called('/mp/matching2/kick_member')[0][0]
        self.assertEqual((kicked['MemberId'], kicked['KickerMemberId']), (2, 1))
        self.assertEqual(kick_reason(kicked['OptData']), bytes([0x19, 0x00, 0x00, 0xff]))

    def test_accounts_and_ruleset(self):
        server, _host, _guest = self.play(accounts={'HostA': 'token-host', 'GuestB': 'token-guest'},
                                          ruleset='bbpc-cheats')
        for path, name, token in (('/mp/matching2/create_room', 'HostA', 'token-host'),
                                  ('/mp/matching2/join_room', 'GuestB', 'token-guest')):
            body, headers = server.called(path)[0]
            self.assertEqual(body['OnlineId'], name)   # the account's name is the online id
            self.assertEqual(headers.get('X-BB-Token') or headers.get('X-Bb-Token'), token)
            self.assertEqual(headers.get('X-BBHost-Ruleset') or headers.get('X-Bbhost-Ruleset'), 'bbpc-cheats')


if __name__ == '__main__':
    unittest.main()
