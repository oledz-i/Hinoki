// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#pragma once

// Ad hoc over the internet ("relay mode").
//
// When the relay is on, every P2P (ad hoc) socket a game opens becomes a real socket on
// 127.0.0.1. Those sockets talk to a bridge thread in this process, and the bridge forwards
// the traffic to the other players through one TCP connection to a relay server
// (tools/adhoc-relay). Games see a virtual IP in 10.77.0.0/16 and the other players' virtual
// IPs, as if everyone were on the same ad hoc network.
//
// Loopback sockets keep everything the emulator already does with host sockets working:
// blocking and non-blocking calls, timeouts, select/epoll and abort.
//
// Datagrams between a game socket and the bridge carry an 8-byte header in front of the game's
// data: u32 virtual IP (network order), u16 port, u16 vport (host order). Going to the bridge it
// names the destination, coming from the bridge it names the sender.
// A stream connection to the bridge starts with a 12-byte header: destination IP, port, vport,
// then the connecting socket's own port and vport.

#include <cstdint>
#include <string>

namespace net_relay {

constexpr uint16_t DEFAULT_PORT = 27360;
constexpr size_t DGRAM_HEADER_SIZE = 8;
constexpr size_t STREAM_HEADER_SIZE = 12;

struct Config {
    std::string host; // relay server name or IP
    uint16_t port = DEFAULT_PORT;
    std::string room; // players only see each other in the same room
    std::string nickname;
    std::string key; // optional; must match the server's RELAY_KEY
};

// Splits "host" or "host:port" (a bare IPv6 address must be written [addr]:port)
bool parse_server(const std::string &text, std::string &host, uint16_t &port);

// Connects to the relay (or keeps the current connection if the config is the same) and waits
// up to timeout_ms for a virtual IP. On failure, error says why in words a player understands.
bool start(const Config &config, int timeout_ms, std::string &error);
void stop();

// Connects in the background (returns at once); errors go to the log
void start_async(const Config &config);

// The relay is switched on for this game. Ad hoc sockets created before the connection is up
// wait for it (wait_active) on first use instead of quietly using the local network.
void set_wanted(bool wanted);
bool wanted();
// Waits up to timeout_ms for a relay that is still connecting; false if it doesn't come up
bool wait_active(int timeout_ms);

// True once the relay has given us a virtual IP. Sockets created while this is true use the relay.
bool active();
uint32_t virtual_ip(); // network order
uint32_t broadcast_ip(); // network order
uint16_t bridge_udp_port(); // host order
uint16_t bridge_tcp_port(); // host order

// A game's P2P datagram socket bound to (port, vport), whose host socket sits on 127.0.0.1:local_port.
// Returns false if that port pair is taken.
bool register_dgram(uint16_t local_port, uint16_t port, uint16_t vport);
// A game's P2P stream socket listening on (port, vport) at 127.0.0.1:local_port
bool register_listener(uint16_t local_port, uint16_t port, uint16_t vport);
// Forgets a game socket's port pair when it closes
void unregister(uint16_t local_port);

// For a stream the bridge connected to a game's listener: who is really on the other end.
// bridge_port is the peer port the game's accept() reported.
bool lookup_accepted(uint16_t bridge_port, uint32_t &ip, uint16_t &port, uint16_t &vport);

// A short line for logs and the settings screen
std::string status();

} // namespace net_relay
