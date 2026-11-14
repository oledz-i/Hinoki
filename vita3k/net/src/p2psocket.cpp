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

#include <net/relay.h>
#include <net/socket.h>
#include <util/adhoc_peers.h>
#include <util/log.h>

#include <bit>
#include <cstring>
#ifndef _WIN32
#include <fcntl.h>
#endif
#include <vector>

static SceNetSockaddr convertP2PToPosix(const SceNetSockaddr *addr) {
    if (!addr) {
        return SceNetSockaddr{};
    }
    SceNetSockaddrIn result = *reinterpret_cast<const SceNetSockaddrIn *>(addr);
    // Convert ports from network to host order
    const uint16_t port = ntohs(result.sin_port);
    const uint16_t vport = ntohs(result.sin_vport);

    // Combine the two ports in host order, then convert back to network order
    result.sin_port = htons(port + vport);
    result.sin_vport = 0; // Clear virtual port since it's not used in Posix
    return std::bit_cast<SceNetSockaddr>(result);
}

static SceNetSockaddr convertPosixToP2P(const SceNetSockaddr *addr) {
    if (!addr) {
        return SceNetSockaddr{};
    }
    SceNetSockaddrIn result = *reinterpret_cast<const SceNetSockaddrIn *>(addr);
    // Attempt to recover original port and virtual port if port was previously combined
    const uint16_t port = ntohs(result.sin_port);
    if (port > SCE_NET_ADHOC_PORT) {
        const uint16_t vport = port - SCE_NET_ADHOC_PORT;
        result.sin_port = htons(SCE_NET_ADHOC_PORT);
        result.sin_vport = htons(vport);
    } else {
        result.sin_port = htons(port);
        result.sin_vport = 0;
    }
    return std::bit_cast<SceNetSockaddr>(result);
}

static int p2pSocketTypeToPosixSocketType(int type) {
    int hostSockType;
    switch (type) {
    case SCE_NET_SOCK_DGRAM_P2P:
        hostSockType = SOCK_DGRAM;
        break;
    case SCE_NET_SOCK_STREAM_P2P:
        hostSockType = SOCK_STREAM;
        break;
    default:
        hostSockType = -1;
    }
    return hostSockType;
}

// ---- Relay mode ----
// See net/relay.h. The game's port pair (port, vport) and the virtual IPs are only known to the
// bridge; the host socket itself is an ordinary socket on 127.0.0.1.

namespace {

SceNetSockaddrIn make_p2p_addr(uint32_t ip_net, uint16_t port, uint16_t vport) {
    SceNetSockaddrIn addr{};
    addr.sin_len = sizeof(SceNetSockaddrIn);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip_net;
    addr.sin_vport = htons(vport);
    return addr;
}

SceNetSockaddr loopback_sce(uint16_t port) {
    SceNetSockaddrIn addr{};
    addr.sin_len = sizeof(SceNetSockaddrIn);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return std::bit_cast<SceNetSockaddr>(addr);
}

void write_addr(SceNetSockaddr *out, unsigned int *outlen, const SceNetSockaddrIn &addr) {
    if (out)
        std::memcpy(out, &addr, sizeof(addr));
    if (outlen)
        *outlen = sizeof(addr);
}

uint16_t host_local_port(abs_socket sock) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len) != 0)
        return 0;
    return ntohs(addr.sin_port);
}

} // namespace

// Binds the host socket to 127.0.0.1 and remembers the game's port pair
static int relay_bind(P2PSocket &s, uint16_t port, uint16_t vport) {
    if (s.relay_bound)
        return SCE_NET_ERROR_EINVAL;
    sockaddr_in host{};
    host.sin_family = AF_INET;
    host.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s.sock, reinterpret_cast<sockaddr *>(&host), sizeof(host)) != 0)
        return PosixSocket::translate_return_value(-1);
    s.relay_local_port = host_local_port(s.sock);
    if (!port && !vport) {
        // Unbound or "any port": make one up, the way the Vita picks a free vport
        port = SCE_NET_ADHOC_PORT;
        vport = static_cast<uint16_t>(0xD000 | (s.relay_local_port & 0x0FFF));
    }
    s.relay_port = port;
    s.relay_vport = vport;
    s.relay_bound = true;
    if (s.sce_type == SCE_NET_SOCK_DGRAM_P2P && !net_relay::register_dgram(s.relay_local_port, port, vport))
        return SCE_NET_ERROR_EADDRINUSE;
    return 0;
}

P2PSocket::P2PSocket(int domain, int type, int protocol)
    : PosixSocket(domain, p2pSocketTypeToPosixSocketType(type), protocol) {
    sce_type = type;
    relay = net_relay::active();
    relay_pending = !relay && net_relay::wanted();
}

void P2PSocket::resolve_relay() {
    if (!relay_pending)
        return;
    relay_pending = false;
    relay = net_relay::wait_active(8000);
}

int P2PSocket::close() {
    if (relay && relay_local_port)
        net_relay::unregister(relay_local_port);
    return PosixSocket::close();
}

int P2PSocket::listen(int backlog) {
    resolve_relay();
    if (!relay) {
        const auto res = PosixSocket::listen(backlog);
        listening = res >= 0;
        return res;
    }
    if (!relay_bound) {
        const auto res = relay_bind(*this, 0, 0);
        if (res < 0)
            return res;
    }
    const auto res = PosixSocket::listen(backlog);
    if (res < 0)
        return res;
    if (!net_relay::register_listener(relay_local_port, relay_port, relay_vport))
        return SCE_NET_ERROR_EADDRINUSE;
    listening = true;
    return 0;
}

int P2PSocket::abort(int flags) {
    const auto res = PosixSocket::abort(flags);
#ifndef _WIN32
    const uint16_t local_port = relay ? relay_local_port : host_local_port(sock);
    if (!local_port || (sce_type != SCE_NET_SOCK_DGRAM_P2P && !listening))
        return res;
    // Abort wakes a thread blocked on this socket with shutdown(). On an unconnected datagram
    // socket or a listening socket, shutdown() fails with ENOTCONN and wakes nothing, so a game
    // that aborts its ad hoc socket to stop a receive thread would wait forever. Poke the socket
    // through loopback instead: the blocked call returns and sees the abort.
    sockaddr_in to{};
    socklen_t to_len = sizeof(to);
    // The socket's own address; loopback reaches it when it is bound to "any" (or in relay mode)
    if (relay || getsockname(sock, reinterpret_cast<sockaddr *>(&to), &to_len) != 0 || to.sin_addr.s_addr == htonl(INADDR_ANY))
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_family = AF_INET;
    to.sin_port = htons(local_port);
    if (sce_type == SCE_NET_SOCK_DGRAM_P2P) {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            ::sendto(fd, "", 0, 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
            ::close(fd);
        }
    } else {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            const int flags_fd = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags_fd | O_NONBLOCK);
            ::connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof(to));
            ::close(fd);
        }
    }
    return 0;
#else
    return res;
#endif
}

SocketPtr P2PSocket::accept(SceNetSockaddr *addr, unsigned int *addrlen, int &err) {
    resolve_relay();
    if (relay) {
        SceNetSockaddr host_addr{};
        unsigned int host_len = sizeof(host_addr);
        const auto res = std::dynamic_pointer_cast<PosixSocket>(PosixSocket::accept(&host_addr, &host_len, err));
        if (!res)
            return nullptr;
        auto sock = std::make_shared<P2PSocket>(res->sock, res->sce_type);
        sock->relay = true;
        const uint16_t bridge_port = ntohs(reinterpret_cast<SceNetSockaddrIn *>(&host_addr)->sin_port);
        uint32_t ip = 0;
        uint16_t port = 0, vport = 0;
        if (net_relay::lookup_accepted(bridge_port, ip, port, vport)) {
            sock->relay_peer_ip = ip;
            sock->relay_peer_port = port;
            sock->relay_peer_vport = vport;
            sock->relay_has_peer = true;
        }
        sock->relay_port = relay_port;
        sock->relay_vport = relay_vport;
        write_addr(addr, addrlen, make_p2p_addr(sock->relay_peer_ip, sock->relay_peer_port, sock->relay_peer_vport));
        return sock;
    }

    const auto res = std::dynamic_pointer_cast<PosixSocket>(PosixSocket::accept(addr, addrlen, err));
    if (!res)
        return nullptr;

    *addr = convertPosixToP2P(addr);
    return std::make_shared<P2PSocket>(res->sock, res->sce_type);
}

int P2PSocket::connect(const SceNetSockaddr *addr, unsigned int addrlen) {
    resolve_relay();
    if (!relay) {
        const auto p2p_addr = convertP2PToPosix(addr);
        return PosixSocket::connect(&p2p_addr, addrlen);
    }
    if (!addr)
        return SCE_NET_ERROR_EINVAL;
    const auto *in = reinterpret_cast<const SceNetSockaddrIn *>(addr);
    relay_peer_ip = in->sin_addr.s_addr;
    relay_peer_port = ntohs(in->sin_port);
    relay_peer_vport = ntohs(in->sin_vport);
    relay_has_peer = true;
    if (sce_type == SCE_NET_SOCK_DGRAM_P2P)
        return 0; // a "connected" datagram socket only remembers its peer

    // Stream: connect to the bridge (always blocking, it's local and instant) and tell it where to
    const int nbio = sockopt_so_nbio;
    if (nbio) {
        int off = 0;
#ifdef _WIN32
        ioctlsocket(sock, FIONBIO, reinterpret_cast<u_long *>(&off));
#else
        ioctl(sock, FIONBIO, &off);
#endif
    }
    const auto bridge = loopback_sce(net_relay::bridge_tcp_port());
    int res = PosixSocket::connect(&bridge, sizeof(bridge));
    if (res >= 0) {
        char header[net_relay::STREAM_HEADER_SIZE];
        std::memcpy(header, &relay_peer_ip, 4);
        std::memcpy(header + 4, &relay_peer_port, 2);
        std::memcpy(header + 6, &relay_peer_vport, 2);
        std::memcpy(header + 8, &relay_port, 2);
        std::memcpy(header + 10, &relay_vport, 2);
        res = PosixSocket::send_packet(header, sizeof(header), 0, nullptr, 0);
        res = res == static_cast<int>(sizeof(header)) ? 0 : (res < 0 ? res : SCE_NET_ERROR_ECONNRESET);
    }
    if (nbio) {
        int on = 1;
#ifdef _WIN32
        ioctlsocket(sock, FIONBIO, reinterpret_cast<u_long *>(&on));
#else
        ioctl(sock, FIONBIO, &on);
#endif
    }
    return res;
}

int P2PSocket::recv_packet(void *buf, unsigned int len, int flags, SceNetSockaddr *from, unsigned int *fromlen) {
    resolve_relay();
    if (relay && sce_type == SCE_NET_SOCK_DGRAM_P2P) {
        if (!relay_bound) {
            const auto r = relay_bind(*this, 0, 0);
            if (r < 0)
                return r;
        }
        thread_local std::vector<char> packet;
        packet.resize(static_cast<size_t>(len) + net_relay::DGRAM_HEADER_SIZE);
        int res;
        while (true) {
            res = PosixSocket::recv_packet(packet.data(), static_cast<unsigned int>(packet.size()), flags, nullptr, nullptr);
            if (res < 0)
                return res;
            if (res >= static_cast<int>(net_relay::DGRAM_HEADER_SIZE))
                break;
            // A leftover wake-up from abort(): not a packet. Wait for a real one if the call blocks.
            if (sockopt_so_nbio || (flags & SCE_NET_MSG_DONTWAIT))
                return SCE_NET_ERROR_EWOULDBLOCK;
        }
        uint32_t ip;
        uint16_t port, vport;
        std::memcpy(&ip, packet.data(), 4);
        std::memcpy(&port, packet.data() + 4, 2);
        std::memcpy(&vport, packet.data() + 6, 2);
        const auto size = std::min<size_t>(len, res - net_relay::DGRAM_HEADER_SIZE);
        std::memcpy(buf, packet.data() + net_relay::DGRAM_HEADER_SIZE, size);
        if (from)
            write_addr(from, fromlen, make_p2p_addr(ip, port, vport));
        return static_cast<int>(size);
    }
    if (relay) {
        const auto res = PosixSocket::recv_packet(buf, len, flags, nullptr, nullptr);
        if (res >= 0 && from)
            write_addr(from, fromlen, make_p2p_addr(relay_peer_ip, relay_peer_port, relay_peer_vport));
        return res;
    }

    const auto res = PosixSocket::recv_packet(buf, len, flags, from, fromlen);
    if (res == 0 && sce_type == SCE_NET_SOCK_DGRAM_P2P && !(flags & SCE_NET_MSG_DONTWAIT) && !sockopt_so_nbio)
        return recv_packet(buf, len, flags, from, fromlen); // an abort() wake-up that came late: not a packet
    if ((res > 0) && from) {
        *from = convertPosixToP2P(from);
        // Whoever sends us ad hoc packets is a player: their broadcasts must reach them too
        const uint32_t sender = reinterpret_cast<const SceNetSockaddrIn *>(from)->sin_addr.s_addr;
        if (adhoc_peers::add(sender))
            LOG_INFO("Ad hoc: found a player at {}.{}.{}.{}", sender & 0xFF, (sender >> 8) & 0xFF, (sender >> 16) & 0xFF, sender >> 24);
    }

    return res;
}

int P2PSocket::send_packet(const void *msg, unsigned int len, int flags, const SceNetSockaddr *to, unsigned int tolen) {
    resolve_relay();
    if (relay && sce_type == SCE_NET_SOCK_DGRAM_P2P) {
        if (!relay_bound) {
            const auto r = relay_bind(*this, 0, 0);
            if (r < 0)
                return r;
        }
        uint32_t ip = relay_peer_ip;
        uint16_t port = relay_peer_port, vport = relay_peer_vport;
        if (to) {
            const auto *in = reinterpret_cast<const SceNetSockaddrIn *>(to);
            ip = in->sin_addr.s_addr;
            port = ntohs(in->sin_port);
            vport = ntohs(in->sin_vport);
        } else if (!relay_has_peer) {
            return SCE_NET_ERROR_EDESTADDRREQ;
        }
        thread_local std::vector<char> packet;
        packet.resize(static_cast<size_t>(len) + net_relay::DGRAM_HEADER_SIZE);
        std::memcpy(packet.data(), &ip, 4);
        std::memcpy(packet.data() + 4, &port, 2);
        std::memcpy(packet.data() + 6, &vport, 2);
        if (len)
            std::memcpy(packet.data() + net_relay::DGRAM_HEADER_SIZE, msg, len);
        const auto bridge = loopback_sce(net_relay::bridge_udp_port());
        const auto res = PosixSocket::send_packet(packet.data(), static_cast<unsigned int>(packet.size()), flags, &bridge, sizeof(sockaddr_in));
        if (res < 0)
            return res;
        return std::max(0, res - static_cast<int>(net_relay::DGRAM_HEADER_SIZE));
    }
    if (relay)
        return PosixSocket::send_packet(msg, len, flags, nullptr, 0);

    const auto p2p_to = convertP2PToPosix(to);
    const uint32_t dest = to ? reinterpret_cast<const SceNetSockaddrIn *>(to)->sin_addr.s_addr : 0;
    if (!to || !adhoc_peers::is_broadcast(dest))
        return PosixSocket::send_packet(msg, len, flags, &p2p_to, tolen);

    // A broadcast: sent as one (where the network allows it) and to every known player directly
    // (iOS drops apps' broadcasts). Succeeds when any copy went out.
    int result = PosixSocket::send_packet(msg, len, flags, &p2p_to, tolen);
    for (const uint32_t peer : adhoc_peers::snapshot()) {
        SceNetSockaddrIn direct = *reinterpret_cast<const SceNetSockaddrIn *>(&p2p_to);
        direct.sin_addr.s_addr = peer;
        const SceNetSockaddr direct_addr = std::bit_cast<SceNetSockaddr>(direct);
        const int sent = PosixSocket::send_packet(msg, len, flags, &direct_addr, tolen);
        if (sent >= 0 && result < 0)
            result = sent;
    }
    return result;
}

int P2PSocket::bind(const SceNetSockaddr *addr, unsigned int addrlen) {
    resolve_relay();
    if (relay) {
        const auto *in = reinterpret_cast<const SceNetSockaddrIn *>(addr);
        return relay_bind(*this, in ? ntohs(in->sin_port) : 0, in ? ntohs(in->sin_vport) : 0);
    }
    const auto p2p_addr = convertP2PToPosix(addr);
    return PosixSocket::bind(&p2p_addr, addrlen);
}

int P2PSocket::get_peer_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    resolve_relay();
    if (relay) {
        if (!addr || !addrlen)
            return SCE_NET_EINVAL;
        if (!relay_has_peer)
            return SCE_NET_ERROR_ENOTCONN;
        write_addr(addr, addrlen, make_p2p_addr(relay_peer_ip, relay_peer_port, relay_peer_vport));
        return 0;
    }
    const auto res = PosixSocket::get_peer_address(addr, addrlen);
    if (res == 0)
        *addr = convertPosixToP2P(addr);

    return res;
}

int P2PSocket::get_socket_address(SceNetSockaddr *addr, unsigned int *addrlen) {
    resolve_relay();
    if (relay) {
        if (!addr || !addrlen)
            return SCE_NET_EINVAL;
        write_addr(addr, addrlen, make_p2p_addr(net_relay::virtual_ip(), relay_port, relay_vport));
        return 0;
    }
    const auto res = PosixSocket::get_socket_address(addr, addrlen);
    if (res == 0)
        *addr = convertPosixToP2P(addr);

    return res;
}
