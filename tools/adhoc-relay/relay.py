#!/usr/bin/env python3
# Hinoki ad hoc relay server
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Puts emulators into rooms, gives each a virtual IP and forwards ad hoc traffic between them.
# Protocol: see PROTOCOL.md. Standard library only.
#
# Settings (environment variables or command-line flags):
#   RELAY_PORT        TCP port to listen on (default 27360)
#   RELAY_KEY         if set, clients must send this key (keeps a public server private)
#   RELAY_MAX_ROOM    players per room (default 8)
#   RELAY_MAX_CLIENTS players on the whole server (default 64)
#   RELAY_RATE        bytes per second per player (default 1000000)

import argparse
import asyncio
import ipaddress
import logging
import os
import struct
import time

VERSION = 1
MAX_FRAME = 70000

HELLO, WELCOME, ERROR, PEER_JOIN, PEER_LEAVE, PING, PONG = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
DGRAM, S_OPEN, S_DATA, S_CLOSE = 0x10, 0x11, 0x13, 0x14
ROUTED = {DGRAM, S_OPEN, S_DATA, S_CLOSE}

SUBNET = int(ipaddress.IPv4Address("10.77.0.0"))
BROADCASTS = {0xFFFFFFFF, SUBNET | 0xFFFF}

log = logging.getLogger("relay")


def ip_str(vip):
    return str(ipaddress.IPv4Address(vip))


def frame(msg_type, body=b""):
    return struct.pack(">IB", len(body) + 1, msg_type) + body


def short_str(data, pos):
    n = data[pos]
    return data[pos + 1:pos + 1 + n].decode("utf-8", "replace"), pos + 1 + n


class Client:
    def __init__(self, server, reader, writer):
        self.server = server
        self.reader = reader
        self.writer = writer
        self.addr = writer.get_extra_info("peername")
        self.room = None
        self.vip = 0
        self.nick = ""
        self.tokens = server.rate
        self.last_refill = time.monotonic()
        self.dropped = 0

    def send(self, data):
        if self.writer.is_closing():
            return
        # A player whose connection backs up more than 2 MB is too slow to keep up: drop it
        transport = self.writer.transport
        if transport.get_write_buffer_size() > 2 * 1024 * 1024:
            log.warning("%s (%s) is not keeping up, disconnecting", self.nick, ip_str(self.vip))
            self.writer.close()
            return
        self.writer.write(data)

    def allow(self, nbytes):
        now = time.monotonic()
        self.tokens = min(self.server.rate, self.tokens + (now - self.last_refill) * self.server.rate)
        self.last_refill = now
        if self.tokens < nbytes:
            return False
        self.tokens -= nbytes
        return True

    async def read_frame(self, timeout):
        header = await asyncio.wait_for(self.reader.readexactly(5), timeout)
        length, msg_type = struct.unpack(">IB", header)
        if length < 1 or length > MAX_FRAME:
            raise ValueError(f"bad frame length {length}")
        body = await asyncio.wait_for(self.reader.readexactly(length - 1), timeout)
        return msg_type, body

    async def run(self):
        try:
            msg_type, body = await self.read_frame(10)
            if msg_type != HELLO or len(body) < 9:
                raise ValueError("expected HELLO")
            version, preferred = struct.unpack_from(">HI", body, 0)
            if version != VERSION:
                self.send(frame(ERROR, f"Relay speaks protocol {VERSION}, Hinoki sent {version}. Update one of them.".encode()))
                return
            room, pos = short_str(body, 6)
            self.nick, pos = short_str(body, pos)
            key, pos = short_str(body, pos)
            if self.server.key and key != self.server.key:
                self.send(frame(ERROR, b"Wrong relay key."))
                log.info("%s: wrong key", self.addr)
                return
            if not room:
                self.send(frame(ERROR, b"No room given."))
                return
            error = self.server.join(self, room, preferred)
            if error:
                self.send(frame(ERROR, error.encode()))
                return

            while True:
                msg_type, body = await self.read_frame(60)
                if msg_type in ROUTED:
                    self.route(msg_type, body)
                elif msg_type == PING:
                    self.send(frame(PONG, body))
                elif msg_type == PONG:
                    pass
                else:
                    log.debug("%s: ignoring message type %#x", self.nick, msg_type)
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError):
            pass
        except ValueError as e:
            log.info("%s: %s", self.addr, e)
        finally:
            self.server.leave(self)
            self.writer.close()

    def route(self, msg_type, body):
        if len(body) < 4:
            return
        if not self.allow(len(body)):
            self.dropped += 1
            if self.dropped % 1000 == 1:
                log.warning("%s (%s) is over the rate limit, dropping packets", self.nick, ip_str(self.vip))
            return
        (dest,) = struct.unpack_from(">I", body, 0)
        out = frame(msg_type, struct.pack(">I", self.vip) + body[4:])
        members = self.server.rooms.get(self.room, {})
        if msg_type == DGRAM and dest in BROADCASTS:
            for vip, other in members.items():
                if vip != self.vip:
                    other.send(out)
            return
        target = members.get(dest)
        if target is not None:
            target.send(out)
        elif msg_type == S_OPEN:
            # Tell the opener at once that nobody is there
            (sid,) = struct.unpack_from(">I", body, 4)
            self.send(frame(S_CLOSE, struct.pack(">II", dest, sid | 0x80000000)))


class Server:
    def __init__(self, key, max_room, max_clients, rate):
        self.key = key
        self.max_room = max_room
        self.max_clients = max_clients
        self.rate = rate
        self.rooms = {}
        self.clients = 0

    def join(self, client, room, preferred):
        if self.clients >= self.max_clients:
            return "The relay is full."
        members = self.rooms.setdefault(room, {})
        if len(members) >= self.max_room:
            return "This room is full."
        vip = preferred if self.valid_vip(preferred) and preferred not in members else self.free_vip(members)
        if vip is None:
            return "No free addresses in this room."
        client.room, client.vip = room, vip
        client.send(frame(WELCOME, struct.pack(">IH", vip, len(members) + 1)))
        for other in members.values():
            other.send(self.join_frame(client))
            client.send(self.join_frame(other))
        members[vip] = client
        self.clients += 1
        log.info("%s joined %s as %s from %s (%d in room)", client.nick or "?", room, ip_str(vip), client.addr[0], len(members))
        return None

    def leave(self, client):
        if client.room is None:
            return
        members = self.rooms.get(client.room, {})
        if members.get(client.vip) is client:
            del members[client.vip]
            self.clients -= 1
            for other in members.values():
                other.send(frame(PEER_LEAVE, struct.pack(">I", client.vip)))
            log.info("%s left %s (%d in room)", client.nick or "?", client.room, len(members))
            if not members:
                del self.rooms[client.room]
        client.room = None

    @staticmethod
    def join_frame(client):
        nick = client.nick.encode()[:255]
        return frame(PEER_JOIN, struct.pack(">IB", client.vip, len(nick)) + nick)

    @staticmethod
    def valid_vip(vip):
        return (vip & 0xFFFF0000) == SUBNET and (vip & 0xFF) not in (0, 255) and ((vip >> 8) & 0xFF) != 255

    def free_vip(self, members):
        for host in range(2, 0xFFFF):
            vip = SUBNET | host
            if self.valid_vip(vip) and vip not in members:
                return vip
        return None


async def main():
    env = os.environ.get
    parser = argparse.ArgumentParser(description="Hinoki ad hoc relay server")
    parser.add_argument("--port", type=int, default=int(env("RELAY_PORT", "27360")))
    parser.add_argument("--host", default=env("RELAY_HOST", "0.0.0.0"))
    parser.add_argument("--key", default=env("RELAY_KEY", ""))
    parser.add_argument("--max-room", type=int, default=int(env("RELAY_MAX_ROOM", "8")))
    parser.add_argument("--max-clients", type=int, default=int(env("RELAY_MAX_CLIENTS", "64")))
    parser.add_argument("--rate", type=int, default=int(env("RELAY_RATE", "1000000")))
    parser.add_argument("--verbose", action="store_true", default=env("RELAY_VERBOSE", "") == "1")
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    server = Server(args.key, args.max_room, args.max_clients, args.rate)

    async def on_connect(reader, writer):
        sock = writer.get_extra_info("socket")
        if sock is not None:
            import socket
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        await Client(server, reader, writer).run()

    listener = await asyncio.start_server(on_connect, args.host, args.port)
    log.info("Hinoki ad hoc relay listening on %s:%d%s", args.host, args.port, " (key required)" if args.key else "")
    async with listener:
        await listener.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
