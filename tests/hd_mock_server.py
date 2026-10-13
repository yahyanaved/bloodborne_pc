# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for the private server's co-op surfaces, as bbhost's client uses them (its
src/net/session.cpp, src/hle/np_matching2.cpp, src/net/stun.cpp and tools/nettest): the
session service's JSON calls, the event queue, and the STUN listener with the relay.

It is the client's view of the server, not the server: what the calls are named, what the
replies carry and what events follow. tests/test_hd_session.py runs two copies of the
backend against it (out/gpu/hd-session-test).
"""
import base64
import http.server
import json
import socket
import struct
import threading

STUN_BINDING_REQUEST, STUN_BINDING_RESPONSE = 0x0001, 0x0101
ATTR_MAPPED_ADDRESS, ATTR_XOR_MAPPED_ADDRESS = 0x0001, 0x8020
ATTR_BBHOST_HELLO, ATTR_BBHOST_RELAY = 0x8100, 0x8101
RELAY_MAGIC = 0xFB


class MockServer:
    def __init__(self, relay=False, accounts=None):
        self.relay = relay
        self.accounts = accounts or {}      # token -> online id (the X-BB-Token header)
        self.lock = threading.RLock()
        self.clients = {}                   # online id -> the endpoint its context_start gave
        self.events = {}                    # online id -> [(id, name, fields)]
        self.next_event = 1
        self.rooms = {}                     # room id -> {'sid', 'max', 'owner', 'members': {mid: record}}
        self.next_room = 1001
        self.calls = []                     # (path, body, headers)
        self.heartbeats = 0
        # STUN and the relay: one UDP socket; a relay port is a number bound to a token.
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('127.0.0.1', 0))
        self.udp.settimeout(0.2)
        self.stun_port = self.udp.getsockname()[1]
        self.relay_by_token = {}            # token -> {'vport', 'addr'}
        self.next_vport = 40001
        self.relayed = 0
        self.stun_requests = 0
        self.running = True
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *_args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
                status, reply = owner.answer(self.path, body, dict(self.headers))
                data = json.dumps(reply).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.base = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        threading.Thread(target=self.udp_loop, daemon=True).start()

    def close(self):
        self.running = False
        self.httpd.shutdown()
        self.httpd.server_close()
        self.udp.close()

    # ---- the session service ---------------------------------------------------------------
    def push(self, online, name, **fields):
        with self.lock:
            self.events.setdefault(online, []).append((self.next_event, name, fields))
            self.next_event += 1

    @staticmethod
    def endpoint(body):
        return {key: body.get(key, default) for key, default in (
            ('LocalAddr', ''), ('LocalPort', 0), ('PublicAddr', ''), ('PublicPort', 0),
            ('MappedAddr', ''), ('MappedPort', 0))}

    @staticmethod
    def member_record(mid, member):
        # The address peers are given: the STUN-mapped one when there is one.
        mapped = member['MappedAddr'] and member['MappedPort']
        return {'MemberId': mid, 'OnlineId': member['OnlineId'],
                'Addr': member['MappedAddr'] if mapped else member['LocalAddr'],
                'Port': member['MappedPort'] if mapped else member['LocalPort'],
                'LocalAddr': member['LocalAddr'], 'LocalPort': member['LocalPort'],
                'MappedAddr': member['MappedAddr'], 'MappedPort': member['MappedPort']}

    def room_of(self, sid):
        for room_id, room in self.rooms.items():
            if room['sid'] == sid:
                return room_id, room
        return 0, None

    def answer(self, path, body, headers):
        with self.lock:
            self.calls.append((path, body, headers))
            token = headers.get('X-BB-Token') or headers.get('X-Bb-Token')
            if self.accounts and path not in ('/np/events/ack', '/np/signaling/resolve'):
                # An account's name is its owner's alone: the token names the player. (A
                # resolve names the peer, not the caller.)
                claimed = body.get('OnlineId')
                if claimed in self.accounts.values() and self.accounts.get(token) != claimed:
                    return 401, {'ResKind': 1, 'Message': 'this name belongs to an account'}
            if path == '/mp/matching2/context_start':
                self.clients[body['OnlineId']] = {
                    'SignalingAddr': body.get('SignalingAddr', ''), 'SignalingPort': body.get('SignalingPort', 0),
                    'MappedAddr': body.get('MappedAddr', ''), 'MappedPort': body.get('MappedPort', 0)}
                return 200, {'ResKind': 0}
            if path == '/np/signaling/resolve':
                client = self.clients.get(body.get('OnlineId'))
                if not client:
                    return 200, {'ResKind': 1, 'Error': 'not registered'}
                mapped = client['MappedAddr'] and client['MappedPort']
                return 200, {'ResKind': 0, 'Addr': client['MappedAddr'] if mapped else client['SignalingAddr'],
                             'Port': client['MappedPort'] if mapped else client['SignalingPort']}
            if path == '/mp/matching2/signaling_update':
                client = self.clients.get(body.get('OnlineId'))
                if client:
                    client.update(MappedAddr=body.get('MappedAddr', ''), MappedPort=body.get('MappedPort', 0))
                return 200, {'ResKind': 0}
            if path == '/mp/matching2/create_room':
                room_id, self.next_room = self.next_room, self.next_room + 1
                self.rooms[room_id] = {'sid': f'sid-{room_id}', 'max': int(body.get('MaxMembers', 5)), 'owner': 1,
                                       'extra': {k: body[k] for k in ('HostArea', 'HostLevel', 'HostPos', 'MemberTag')
                                                 if k in body},
                                       'members': {1: {'OnlineId': body['OnlineId'], **self.endpoint(body)}}}
                return 200, {'ResKind': 0, 'RoomId': room_id, 'SessionId': f'sid-{room_id}', 'MemberId': 1,
                             'MaxMembers': self.rooms[room_id]['max']}
            if path == '/mp/matching2/join_room':
                room = self.rooms.get(int(body.get('RoomId', 0)))
                if not room:
                    return 200, {'ResKind': 1, 'Error': 'Room not found'}
                if len(room['members']) >= room['max']:
                    return 200, {'ResKind': 1, 'Error': 'Room full'}
                mid = max(room['members']) + 1
                joiner = {'OnlineId': body['OnlineId'], **self.endpoint(body)}
                others = [self.member_record(m, rec) for m, rec in room['members'].items()]
                for rec in room['members'].values():
                    self.push(rec['OnlineId'], 'room_member_joined', RoomId=int(body['RoomId']),
                              **self.member_record(mid, joiner))
                room['members'][mid] = joiner
                return 200, {'ResKind': 0, 'SessionId': room['sid'], 'MemberId': mid,
                             'OwnerMemberId': room['owner'], 'MaxMembers': room['max'], 'Members': others}
            if path == '/mp/matching2/leave_room':
                room_id, room = self.room_of(body.get('SessionId'))
                mid = int(body.get('MemberId', 0))
                if room and mid in room['members']:
                    gone = room['members'].pop(mid)
                    for rec in room['members'].values():
                        if mid == room['owner']:
                            self.push(rec['OnlineId'], 'room_closed', RoomId=room_id, Reason='host_left')
                        else:
                            self.push(rec['OnlineId'], 'room_member_left', RoomId=room_id, MemberId=mid,
                                      OnlineId=gone['OnlineId'], Reason='leave_room')
                    if mid == room['owner'] or not room['members']:
                        del self.rooms[room_id]
                return 200, {'ResKind': 0}
            if path == '/mp/matching2/heartbeat':
                self.heartbeats += 1
                _room_id, room = self.room_of(body.get('SessionId'))
                return 200, {'ResKind': 0, 'InRoom': int(bool(room and int(body.get('MemberId', 0)) in room['members']))}
            if path == '/mp/matching2/kick_member':
                room_id, room = self.room_of(body.get('SessionId'))
                mid = int(body.get('MemberId', 0))
                if not room or mid not in room['members'] or int(body.get('KickerMemberId', 0)) != room['owner']:
                    return 200, {'ResKind': 1, 'Error': 'not allowed'}
                gone = room['members'].pop(mid)
                self.push(gone['OnlineId'], 'room_member_kicked', RoomId=room_id, MemberId=mid,
                          OptData=body.get('OptData', ''))
                for rec in room['members'].values():
                    self.push(rec['OnlineId'], 'room_member_left', RoomId=room_id, MemberId=mid,
                              OnlineId=gone['OnlineId'], Reason='kicked')
                return 200, {'ResKind': 0}
            if path == '/np/events/poll':
                cursor = int(body.get('Cursor', 0))
                for event_id, name, fields in self.events.get(body.get('OnlineId'), []):
                    if event_id > cursor:
                        return 200, {'ResKind': 0, 'HasEvent': 1, 'EventId': event_id, 'Name': name,
                                     'NextCursor': event_id, **fields}
                return 200, {'ResKind': 0, 'HasEvent': 0, 'NextCursor': cursor}
            if path == '/np/events/ack':
                return 200, {'ResKind': 0}
            return 404, {'detail': 'Not Found'}

    def called(self, path):
        with self.lock:
            return [(body, headers) for p, body, headers in self.calls if p == path]

    # ---- STUN and the relay ----------------------------------------------------------------
    def udp_loop(self):
        while self.running:
            try:
                data, src = self.udp.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                if len(data) >= 12 and data[0] == RELAY_MAGIC and data[1] == ord('R'):
                    self.relay_frame(data, src)
                elif len(data) >= 20 and struct.unpack_from('!H', data)[0] == STUN_BINDING_REQUEST:
                    self.stun(data, src)
            except (struct.error, OSError):
                pass

    def stun(self, data, src):
        _type, length = struct.unpack_from('!HH', data)
        txid = data[4:20]
        token = None
        at = 20
        while at + 4 <= 20 + length:
            kind, size = struct.unpack_from('!HH', data, at)
            value = data[at + 4:at + 4 + size]
            if kind == ATTR_BBHOST_HELLO and size >= 12 and value[:4] == b'bbr1':
                token = value[4:12]
            at += 4 + ((size + 3) & ~3)
        with self.lock:
            self.stun_requests += 1
            mapped = src
            relay_attr = b''
            if self.relay and token is not None:
                if token not in self.relay_by_token:   # zeros, or a token from before a restart
                    token = struct.pack('!Q', 0x1122334455660000 + self.next_vport)
                    self.relay_by_token[token] = {'vport': self.next_vport}
                    self.next_vport += 1
                entry = self.relay_by_token[token]
                entry['addr'] = src                     # where its frames come from now
                mapped = ('127.0.0.1', entry['vport'])  # the relay port is what peers are given
                value = b'bbr1' + token + struct.pack('!H', entry['vport']) + socket.inet_aton(src[0]) + \
                    struct.pack('!H', src[1])
                relay_attr = struct.pack('!HH', ATTR_BBHOST_RELAY, len(value)) + value
        ip = socket.inet_aton(mapped[0])
        plain = struct.pack('!HHBBH', ATTR_MAPPED_ADDRESS, 8, 0, 1, mapped[1]) + ip
        # XOR-MAPPED-ADDRESS as the private server writes it: xored with the transaction id.
        xport = mapped[1] ^ struct.unpack_from('!H', txid)[0]
        xip = struct.pack('!I', struct.unpack('!I', ip)[0] ^ struct.unpack_from('!I', txid)[0])
        xored = struct.pack('!HHBBH', ATTR_XOR_MAPPED_ADDRESS, 8, 0, 1, xport) + xip
        attrs = plain + xored + relay_attr
        self.udp.sendto(struct.pack('!HH', STUN_BINDING_RESPONSE, len(attrs)) + txid + attrs, src)

    def relay_frame(self, data, src):
        token, dst_vport = data[2:10], struct.unpack_from('!H', data, 10)[0]
        with self.lock:
            sender = self.relay_by_token.get(token)
            if not sender:
                return
            sender['addr'] = src
            target = next((e for e in self.relay_by_token.values() if e['vport'] == dst_vport), None)
            if not target or 'addr' not in target:
                return
            self.relayed += 1
            frame = bytes([RELAY_MAGIC, ord('r')]) + struct.pack('!H', sender['vport']) + data[12:]
            where = target['addr']
        self.udp.sendto(frame, where)


def kick_reason(opt_data):
    """The 4-byte reason a host passed with a kick, as the events carry it."""
    return base64.b64decode(opt_data)
