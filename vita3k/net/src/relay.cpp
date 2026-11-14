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

// Ad hoc relay client: the bridge between the game's loopback P2P sockets and the relay server.
// See net/relay.h for the design and tools/adhoc-relay/PROTOCOL.md for the wire format.

#include <net/relay.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

#ifdef NET_RELAY_STANDALONE
#include <cstdio>
#define RELAY_INFO(msg) std::fprintf(stderr, "[relay] %s\n", std::string(msg).c_str())
#define RELAY_WARN(msg) std::fprintf(stderr, "[relay] warning: %s\n", std::string(msg).c_str())
#define RELAY_DEBUG(msg) std::fprintf(stderr, "[relay] %s\n", std::string(msg).c_str())
#else
#include <util/log.h>
#define RELAY_INFO(msg) LOG_INFO("Ad hoc relay: {}", std::string(msg))
#define RELAY_WARN(msg) LOG_WARN("Ad hoc relay: {}", std::string(msg))
#define RELAY_DEBUG(msg) LOG_DEBUG("Ad hoc relay: {}", std::string(msg))
#endif

namespace net_relay {

bool parse_server(const std::string &input, std::string &host, uint16_t &port) {
    std::string text = input;
    text.erase(0, text.find_first_not_of(" \t\r\n"));
    text.erase(text.find_last_not_of(" \t\r\n") + 1);
    port = DEFAULT_PORT;
    std::string port_text;
    if (text.empty())
        return false;
    if (text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string::npos)
            return false;
        host = text.substr(1, close - 1);
        if (close + 1 < text.size()) {
            if (text[close + 1] != ':')
                return false;
            port_text = text.substr(close + 2);
        }
    } else if (std::count(text.begin(), text.end(), ':') == 1) {
        const auto colon = text.find(':');
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
    } else {
        host = text; // a name, an IPv4 address or a bare IPv6 address
    }
    if (!port_text.empty()) {
        char *end = nullptr;
        const long value = std::strtol(port_text.c_str(), &end, 10);
        if (!end || *end || value <= 0 || value > 65535)
            return false;
        port = static_cast<uint16_t>(value);
    }
    return !host.empty();
}

} // namespace net_relay

#ifdef _WIN32

// Relay mode is only built for Unix-like hosts (iOS, macOS, Linux, Android)
namespace net_relay {
void start_async(const Config &) {}
void set_wanted(bool) {}
bool wanted() { return false; }
bool wait_active(int) { return false; }
bool start(const Config &, int, std::string &error) {
    error = "Ad hoc over the internet is not supported on this platform.";
    return false;
}
void stop() {}
bool active() { return false; }
uint32_t virtual_ip() { return 0; }
uint32_t broadcast_ip() { return 0; }
uint16_t bridge_udp_port() { return 0; }
uint16_t bridge_tcp_port() { return 0; }
bool register_dgram(uint16_t, uint16_t, uint16_t) { return false; }
bool register_listener(uint16_t, uint16_t, uint16_t) { return false; }
void unregister(uint16_t) {}
bool lookup_accepted(uint16_t, uint32_t &, uint16_t &, uint16_t &) { return false; }
std::string status() { return "not supported"; }
} // namespace net_relay

#else

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace net_relay {
namespace {

enum MsgType : uint8_t {
    HELLO = 0x01,
    WELCOME = 0x02,
    ERROR_MSG = 0x03,
    PEER_JOIN = 0x04,
    PEER_LEAVE = 0x05,
    PING = 0x06,
    PONG = 0x07,
    DGRAM = 0x10,
    S_OPEN = 0x11,
    S_DATA = 0x13,
    S_CLOSE = 0x14,
};

constexpr uint16_t PROTOCOL_VERSION = 1;
constexpr uint32_t ACCEPTOR_BIT = 0x80000000u;
constexpr uint32_t SUBNET = 0x0A4D0000u; // 10.77.0.0, host order
constexpr size_t MAX_FRAME = 70000;
constexpr size_t SERVER_BACKLOG_LIMIT = 2 * 1024 * 1024; // drop datagrams beyond this
constexpr size_t STREAM_READ_PAUSE = 1024 * 1024; // stop reading game streams beyond this
constexpr uint16_t ADHOC_PORT = 3658;

using Clock = std::chrono::steady_clock;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

std::string ip_text(uint32_t host_order_ip) {
    return std::to_string(host_order_ip >> 24) + "." + std::to_string((host_order_ip >> 16) & 0xFF) + "."
        + std::to_string((host_order_ip >> 8) & 0xFF) + "." + std::to_string(host_order_ip & 0xFF);
}

void put8(std::string &out, uint8_t v) { out.push_back(static_cast<char>(v)); }
void put16(std::string &out, uint16_t v) {
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v));
}
void put32(std::string &out, uint32_t v) {
    put16(out, static_cast<uint16_t>(v >> 16));
    put16(out, static_cast<uint16_t>(v));
}
void put64(std::string &out, uint64_t v) {
    put32(out, static_cast<uint32_t>(v >> 32));
    put32(out, static_cast<uint32_t>(v));
}
void put_short_string(std::string &out, const std::string &s) {
    const auto n = std::min<size_t>(s.size(), 255);
    put8(out, static_cast<uint8_t>(n));
    out.append(s, 0, n);
}
uint16_t get16(const char *p) {
    return static_cast<uint16_t>((static_cast<uint8_t>(p[0]) << 8) | static_cast<uint8_t>(p[1]));
}
uint32_t get32(const char *p) { return (uint32_t(get16(p)) << 16) | get16(p + 2); }
uint64_t get64(const char *p) { return (uint64_t(get32(p)) << 32) | get32(p + 4); }

std::string make_frame(uint8_t type, const std::string &body) {
    std::string out;
    out.reserve(5 + body.size());
    put32(out, static_cast<uint32_t>(body.size() + 1));
    put8(out, type);
    out += body;
    return out;
}

void set_nonblocking(int fd, bool on) {
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

void no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

void close_fd(int &fd) {
    if (fd >= 0)
        ::close(fd);
    fd = -1;
}

bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

sockaddr_in loopback(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

uint16_t local_port_of(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0)
        return 0;
    return ntohs(addr.sin_port);
}

struct PortPair {
    uint16_t port = 0;
    uint16_t vport = 0;
    bool operator<(const PortPair &o) const { return std::tie(port, vport) < std::tie(o.port, o.vport); }
};

std::string pair_text(const PortPair &p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u/0x%04X", p.port, p.vport);
    return buf;
}

struct Accepted {
    uint32_t ip; // host order
    PortPair pair;
};

struct Stream {
    int fd = -1;
    uint32_t peer = 0; // host order
    uint32_t sid = 0; // without ACCEPTOR_BIT
    bool initiator = false; // this side connected
    bool header_done = false;
    std::string header;
    std::string out; // waiting to be written to the game
    bool remote_closed = false;
    uint16_t accepted_port = 0; // bridge-side port recorded in the accepted map
};

using StreamKey = std::tuple<uint32_t, uint32_t, bool>; // peer, sid, initiator

bool same_config(const Config &a, const Config &b) {
    return a.host == b.host && a.port == b.port && a.room == b.room && a.nickname == b.nickname && a.key == b.key;
}

class Link {
public:
    ~Link() { stop(); }

    bool start(const Config &config, int timeout_ms, std::string &error) {
        std::lock_guard<std::mutex> guard(start_mutex);
        if (running && have_vip && same_config(config, cfg))
            return true;
        stop_locked();

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            cfg = config;
        }
        if (!open_local_sockets(error)) {
            close_local_sockets();
            return false;
        }
        server_fd = connect_server(timeout_ms, 0, error);
        if (server_fd < 0) {
            close_local_sockets();
            return false;
        }
        running = true;
        have_vip = true;
        thread = std::thread([this] { loop(); });
        RELAY_INFO("connected to " + config.host + ":" + std::to_string(config.port) + ", room \"" + config.room + "\", virtual IP " + ip_text(vip));
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> guard(start_mutex);
        stop_locked();
    }

    bool is_active() const { return running && have_vip; }
    uint32_t virtual_ip() const { return htonl(vip.load()); }
    uint16_t udp_port() const { return bridge_udp; }
    uint16_t tcp_port() const { return bridge_tcp; }

    bool register_dgram(uint16_t local_port, PortPair pair) {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (dgram_by_pair.count(pair)) {
            RELAY_WARN("game datagram socket " + pair_text(pair) + " is already taken");
            return false;
        }
        dgram_by_pair[pair] = local_port;
        dgram_by_local[local_port] = pair;
        RELAY_DEBUG("game opened datagram socket " + pair_text(pair));
        return true;
    }

    bool register_listener(uint16_t local_port, PortPair pair) {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (listener_by_pair.count(pair)) {
            RELAY_WARN("game stream listener " + pair_text(pair) + " is already taken");
            return false;
        }
        listener_by_pair[pair] = local_port;
        listener_by_local[local_port] = pair;
        RELAY_DEBUG("game listens for streams on " + pair_text(pair));
        return true;
    }

    void unregister(uint16_t local_port) {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (const auto it = dgram_by_local.find(local_port); it != dgram_by_local.end()) {
            RELAY_DEBUG("game closed datagram socket " + pair_text(it->second));
            dgram_by_pair.erase(it->second);
            dgram_by_local.erase(it);
        }
        if (const auto it = listener_by_local.find(local_port); it != listener_by_local.end()) {
            RELAY_DEBUG("game stopped listening on " + pair_text(it->second));
            listener_by_pair.erase(it->second);
            listener_by_local.erase(it);
        }
    }

    bool lookup_accepted(uint16_t bridge_port, Accepted &out) {
        std::lock_guard<std::mutex> lock(state_mutex);
        const auto it = accepted.find(bridge_port);
        if (it == accepted.end())
            return false;
        out = it->second;
        return true;
    }

    std::string status() {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!running)
            return "off";
        std::string s = (server_up ? "connected to " : "reconnecting to ") + cfg.host + ":" + std::to_string(cfg.port);
        s += ", room \"" + cfg.room + "\", virtual IP " + ip_text(vip) + ", " + std::to_string(peers.size()) + " other player(s)";
        if (last_rtt_ms >= 0)
            s += ", ping " + std::to_string(last_rtt_ms) + " ms";
        return s;
    }

private:
    std::mutex start_mutex;
    std::mutex state_mutex; // guards cfg, the port maps, accepted, peers, server_up, last_rtt_ms
    Config cfg;

    std::map<PortPair, uint16_t> dgram_by_pair;
    std::map<uint16_t, PortPair> dgram_by_local;
    std::map<PortPair, uint16_t> listener_by_pair;
    std::map<uint16_t, PortPair> listener_by_local;
    std::map<uint16_t, Accepted> accepted;
    std::map<uint32_t, std::string> peers; // host-order IP -> nickname
    bool server_up = false;
    int64_t last_rtt_ms = -1;

    std::atomic<bool> running{ false };
    std::atomic<bool> have_vip{ false };
    std::atomic<uint32_t> vip{ 0 }; // host order
    uint16_t bridge_udp = 0;
    uint16_t bridge_tcp = 0;
    std::thread thread;

    // Owned by the bridge thread (and by start() before the thread exists)
    int server_fd = -1;
    int udp_fd = -1;
    int listen_fd = -1;
    int wake_fds[2] = { -1, -1 };
    std::string inbuf;
    std::string outbuf;
    std::map<int, Stream> streams; // by local fd
    std::map<StreamKey, int> stream_by_key;
    uint32_t next_sid = 1;
    uint16_t next_vport = 0xC000;
    int64_t last_recv = 0;
    int64_t last_ping = 0;
    int64_t last_rtt_log = 0;
    int64_t next_reconnect = 0;
    void stop_locked() {
        if (running) {
            running = false;
            const char c = 0;
            [[maybe_unused]] const auto r = ::write(wake_fds[1], &c, 1);
            if (thread.joinable())
                thread.join();
            RELAY_INFO("disconnected");
        }
        have_vip = false;
        close_server();
        close_local_sockets();
        std::lock_guard<std::mutex> lock(state_mutex);
        accepted.clear();
        peers.clear();
    }

    bool open_local_sockets(std::string &error) {
        if (pipe(wake_fds) != 0) {
            error = "could not create the relay wake-up pipe";
            return false;
        }
        set_nonblocking(wake_fds[0], true);

        udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        auto addr = loopback(0);
        if (udp_fd < 0 || bind(udp_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            error = "could not open the local relay socket";
            return false;
        }
        const int buf = 1024 * 1024;
        setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
        setsockopt(udp_fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
        set_nonblocking(udp_fd, true);
        bridge_udp = local_port_of(udp_fd);

        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        addr = loopback(0);
        if (listen_fd < 0 || bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(listen_fd, 32) != 0) {
            error = "could not open the local relay listener";
            return false;
        }
        set_nonblocking(listen_fd, true);
        bridge_tcp = local_port_of(listen_fd);
        return true;
    }

    void close_local_sockets() {
        close_fd(udp_fd);
        close_fd(listen_fd);
        close_fd(wake_fds[0]);
        close_fd(wake_fds[1]);
        bridge_udp = bridge_tcp = 0;
    }

    void close_server() {
        close_fd(server_fd);
        inbuf.clear();
        outbuf.clear();
        // Streams can't survive a new connection: the other side forgets them too
        while (!streams.empty())
            close_stream(streams.begin()->first, false);
        std::lock_guard<std::mutex> lock(state_mutex);
        server_up = false;
        peers.clear();
    }

    // Blocking connect + handshake with a deadline. Returns the socket (non-blocking) or -1.
    int connect_server(int timeout_ms, uint32_t preferred_vip, std::string &error) {
        Config c;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            c = cfg;
        }
        const int64_t deadline = now_ms() + timeout_ms;
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *results = nullptr;
        const std::string port_text = std::to_string(c.port);
        if (getaddrinfo(c.host.c_str(), port_text.c_str(), &hints, &results) != 0 || !results) {
            error = "could not find the relay server \"" + c.host + "\" (check the address and your internet connection)";
            return -1;
        }

        int fd = -1;
        for (addrinfo *ai = results; ai && fd < 0; ai = ai->ai_next) {
            fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
                continue;
            no_sigpipe(fd);
            set_nonblocking(fd, true);
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS) {
                close_fd(fd);
                continue;
            }
            pollfd p{ fd, POLLOUT, 0 };
            const int wait = static_cast<int>(std::max<int64_t>(0, deadline - now_ms()));
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (poll(&p, 1, wait) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0)
                close_fd(fd);
        }
        freeaddrinfo(results);
        if (fd < 0) {
            error = "could not reach the relay server at " + c.host + ":" + port_text + " (is it running, and is the port open?)";
            return -1;
        }
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        std::string hello;
        put16(hello, PROTOCOL_VERSION);
        put32(hello, preferred_vip);
        put_short_string(hello, c.room);
        put_short_string(hello, c.nickname);
        put_short_string(hello, c.key);
        const std::string out = make_frame(HELLO, hello);
        size_t sent = 0;
        while (sent < out.size()) {
            const auto n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
            if (n > 0) {
                sent += n;
                continue;
            }
            pollfd p{ fd, POLLOUT, 0 };
            if ((n < 0 && !would_block()) || poll(&p, 1, static_cast<int>(std::max<int64_t>(0, deadline - now_ms()))) != 1) {
                error = "the relay server closed the connection";
                close_fd(fd);
                return -1;
            }
        }

        // Wait for WELCOME (or ERROR)
        std::string buf;
        while (true) {
            if (buf.size() >= 5) {
                const uint32_t len = get32(buf.data());
                if (len < 1 || len > MAX_FRAME) {
                    error = "the relay server sent something unexpected (is that address really a Hinoki relay?)";
                    break;
                }
                if (buf.size() >= 4 + len) {
                    const uint8_t type = static_cast<uint8_t>(buf[4]);
                    const std::string body = buf.substr(5, len - 1);
                    buf.erase(0, 4 + len);
                    if (type == WELCOME && body.size() >= 6) {
                        vip = get32(body.data());
                        inbuf = buf; // PEER_JOINs may already be here
                        outbuf.clear();
                        last_recv = last_ping = now_ms();
                        std::lock_guard<std::mutex> lock(state_mutex);
                        server_up = true;
                        return fd;
                    }
                    if (type == ERROR_MSG) {
                        error = "relay server: " + body;
                        break;
                    }
                    continue;
                }
            }
            pollfd p{ fd, POLLIN, 0 };
            const int wait = static_cast<int>(std::max<int64_t>(0, deadline - now_ms()));
            if (poll(&p, 1, wait) != 1) {
                error = "the relay server did not answer in time";
                break;
            }
            char tmp[4096];
            const auto n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0)
                buf.append(tmp, n);
            else if (n == 0 || !would_block()) {
                error = "the relay server closed the connection";
                break;
            }
        }
        close_fd(fd);
        return -1;
    }

    // ---- server traffic ----

    void send_frame(uint8_t type, const std::string &body) {
        if (server_fd < 0)
            return;
        if (type == DGRAM && outbuf.size() > SERVER_BACKLOG_LIMIT)
            return; // the connection is backed up: losing a datagram is fine, ad hoc expects it
        outbuf += make_frame(type, body);
        flush_server();
    }

    void flush_server() {
        while (server_fd >= 0 && !outbuf.empty()) {
            const auto n = ::send(server_fd, outbuf.data(), outbuf.size(), MSG_NOSIGNAL);
            if (n > 0) {
                outbuf.erase(0, n);
            } else {
                if (n < 0 && would_block())
                    return;
                drop_server("connection lost while sending");
                return;
            }
        }
    }

    void drop_server(const std::string &why) {
        if (server_fd < 0)
            return;
        RELAY_WARN(why + ", reconnecting");
        close_server();
        next_reconnect = now_ms() + 2000;
    }

    void read_server() {
        char tmp[16384];
        while (server_fd >= 0) {
            const auto n = ::recv(server_fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                inbuf.append(tmp, n);
                last_recv = now_ms();
                continue;
            }
            if (n < 0 && would_block())
                break;
            drop_server("the relay server closed the connection");
            return;
        }
        size_t pos = 0;
        while (server_fd >= 0 && inbuf.size() - pos >= 5) {
            const uint32_t len = get32(inbuf.data() + pos);
            if (len < 1 || len > MAX_FRAME) {
                drop_server("bad data from the relay server");
                return;
            }
            if (inbuf.size() - pos < 4 + len)
                break;
            const uint8_t type = static_cast<uint8_t>(inbuf[pos + 4]);
            const std::string body = inbuf.substr(pos + 5, len - 1);
            pos += 4 + len;
            handle_frame(type, body);
        }
        if (server_fd >= 0)
            inbuf.erase(0, pos);
    }

    void handle_frame(uint8_t type, const std::string &body) {
        const char *p = body.data();
        switch (type) {
        case PING:
            send_frame(PONG, body);
            break;
        case PONG:
            if (body.size() >= 8) {
                const int64_t rtt = now_ms() - static_cast<int64_t>(get64(p));
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    last_rtt_ms = rtt;
                }
                if (now_ms() - last_rtt_log > 60000) {
                    last_rtt_log = now_ms();
                    RELAY_INFO("ping to relay " + std::to_string(rtt) + " ms");
                }
            }
            break;
        case PEER_JOIN:
            if (body.size() >= 5) {
                const uint32_t ip = get32(p);
                const auto n = std::min<size_t>(static_cast<uint8_t>(p[4]), body.size() - 5);
                const std::string nick = body.substr(5, n);
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    peers[ip] = nick;
                }
                RELAY_INFO("player \"" + nick + "\" is in the room (" + ip_text(ip) + ")");
            }
            break;
        case PEER_LEAVE:
            if (body.size() >= 4) {
                const uint32_t ip = get32(p);
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    peers.erase(ip);
                }
                RELAY_INFO("player " + ip_text(ip) + " left");
                std::vector<int> gone;
                for (const auto &[fd, s] : streams)
                    if (s.peer == ip)
                        gone.push_back(fd);
                for (const int fd : gone)
                    close_stream(fd, false);
            }
            break;
        case ERROR_MSG:
            RELAY_WARN("relay server: " + body);
            break;
        case DGRAM:
            if (body.size() >= 12)
                deliver_dgram(get32(p), { get16(p + 4), get16(p + 6) }, { get16(p + 8), get16(p + 10) }, p + 12, body.size() - 12);
            break;
        case S_OPEN:
            if (body.size() >= 16)
                remote_open(get32(p), get32(p + 4) & ~ACCEPTOR_BIT, { get16(p + 8), get16(p + 10) }, { get16(p + 12), get16(p + 14) });
            break;
        case S_DATA:
            if (body.size() >= 8) {
                const uint32_t sid = get32(p + 4);
                const auto it = stream_by_key.find({ get32(p), sid & ~ACCEPTOR_BIT, (sid & ACCEPTOR_BIT) != 0 });
                if (it != stream_by_key.end()) {
                    auto &s = streams[it->second];
                    s.out.append(p + 8, body.size() - 8);
                    flush_stream(it->second);
                }
            }
            break;
        case S_CLOSE:
            if (body.size() >= 8) {
                const uint32_t sid = get32(p + 4);
                const auto it = stream_by_key.find({ get32(p), sid & ~ACCEPTOR_BIT, (sid & ACCEPTOR_BIT) != 0 });
                if (it != stream_by_key.end()) {
                    const int fd = it->second;
                    streams[fd].remote_closed = true;
                    if (streams[fd].out.empty())
                        close_stream(fd, false);
                }
            }
            break;
        default:
            break;
        }
    }

    // A datagram for one of our game sockets: hand it over with the sender in front
    void deliver_dgram(uint32_t from_ip, PortPair from, PortPair to, const char *data, size_t size) {
        uint16_t local_port = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            auto it = dgram_by_pair.find(to);
            if (it == dgram_by_pair.end()) {
                // Ad hoc sockets are told apart by vport; accept any port with the same vport
                it = std::find_if(dgram_by_pair.begin(), dgram_by_pair.end(), [&](const auto &e) { return e.first.vport == to.vport; });
            }
            if (it == dgram_by_pair.end()) {
                RELAY_DEBUG("datagram from " + ip_text(from_ip) + " " + pair_text(from) + " to " + pair_text(to) + ": no game socket there, dropped");
                return;
            }
            local_port = it->second;
        }
        std::string packet(DGRAM_HEADER_SIZE, '\0');
        const uint32_t ip_net = htonl(from_ip);
        std::memcpy(packet.data(), &ip_net, 4);
        std::memcpy(packet.data() + 4, &from.port, 2);
        std::memcpy(packet.data() + 6, &from.vport, 2);
        packet.append(data, size);
        const auto dest = loopback(local_port);
        ::sendto(udp_fd, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr *>(&dest), sizeof(dest));
    }

    // A datagram from one of our game sockets
    void read_udp() {
        static thread_local std::string buf(MAX_FRAME, '\0');
        while (true) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const auto n = ::recvfrom(udp_fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
            if (n < 0)
                return;
            if (static_cast<size_t>(n) < DGRAM_HEADER_SIZE)
                continue;
            PortPair src;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                const auto it = dgram_by_local.find(ntohs(from.sin_port));
                if (it == dgram_by_local.end())
                    continue;
                src = it->second;
            }
            uint32_t dst_net;
            PortPair dst;
            std::memcpy(&dst_net, buf.data(), 4);
            std::memcpy(&dst.port, buf.data() + 4, 2);
            std::memcpy(&dst.vport, buf.data() + 6, 2);
            const uint32_t dst_ip = ntohl(dst_net);
            const char *payload = buf.data() + DGRAM_HEADER_SIZE;
            const size_t size = n - DGRAM_HEADER_SIZE;
            if (dst_ip == vip) {
                deliver_dgram(vip, src, dst, payload, size);
                continue;
            }
            std::string body;
            body.reserve(12 + size);
            put32(body, dst_ip);
            put16(body, src.port);
            put16(body, src.vport);
            put16(body, dst.port);
            put16(body, dst.vport);
            body.append(payload, size);
            send_frame(DGRAM, body);
        }
    }

    // ---- streams ----

    void add_stream(Stream s) {
        const int fd = s.fd;
        if (s.header_done)
            stream_by_key[{ s.peer, s.sid, s.initiator }] = fd;
        streams[fd] = std::move(s);
    }

    void close_stream(int fd, bool tell_remote) {
        const auto it = streams.find(fd);
        if (it == streams.end())
            return;
        // Forget the stream completely before telling the server: a failed send can drop the
        // connection, which closes every stream again
        const Stream s = std::move(it->second);
        streams.erase(it);
        if (s.header_done)
            stream_by_key.erase({ s.peer, s.sid, s.initiator });
        if (s.accepted_port) {
            std::lock_guard<std::mutex> lock(state_mutex);
            accepted.erase(s.accepted_port);
        }
        ::close(s.fd);
        if (tell_remote && s.header_done) {
            std::string body;
            put32(body, s.peer);
            put32(body, s.sid | (s.initiator ? 0 : ACCEPTOR_BIT));
            send_frame(S_CLOSE, body);
        }
    }

    // A game connected to the bridge's listener: an outgoing stream
    void accept_local() {
        while (true) {
            const int fd = ::accept(listen_fd, nullptr, nullptr);
            if (fd < 0)
                return;
            set_nonblocking(fd, true);
            no_sigpipe(fd);
            const int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            Stream s;
            s.fd = fd;
            s.initiator = true;
            add_stream(std::move(s));
        }
    }

    // Another player connects to one of our game's listening sockets
    void remote_open(uint32_t peer, uint32_t sid, PortPair from, PortPair to) {
        uint16_t listener_port = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            auto it = listener_by_pair.find(to);
            if (it == listener_by_pair.end())
                it = std::find_if(listener_by_pair.begin(), listener_by_pair.end(), [&](const auto &e) { return e.first.vport == to.vport; });
            if (it != listener_by_pair.end())
                listener_port = it->second;
        }
        const auto refuse = [&] {
            std::string body;
            put32(body, peer);
            put32(body, sid | ACCEPTOR_BIT);
            send_frame(S_CLOSE, body);
        };
        if (!listener_port) {
            RELAY_DEBUG("stream from " + ip_text(peer) + " to " + pair_text(to) + ": nothing listening there, refused");
            return refuse();
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        auto addr = loopback(0);
        if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            close_fd(fd);
            return refuse();
        }
        no_sigpipe(fd);
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const uint16_t bport = local_port_of(fd);
        {
            // Recorded before connecting, so the game's accept() can always find it
            std::lock_guard<std::mutex> lock(state_mutex);
            accepted[bport] = { peer, from };
        }
        const auto dest = loopback(listener_port);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&dest), sizeof(dest)) != 0) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                accepted.erase(bport);
            }
            close_fd(fd);
            return refuse();
        }
        set_nonblocking(fd, true);
        Stream s;
        s.fd = fd;
        s.peer = peer;
        s.sid = sid;
        s.initiator = false;
        s.header_done = true;
        s.accepted_port = bport;
        add_stream(std::move(s));
        RELAY_DEBUG("stream from " + ip_text(peer) + " " + pair_text(from) + " accepted on " + pair_text(to));
    }

    void read_stream(int fd) {
        Stream &s = streams[fd];
        if (!s.header_done) {
            char tmp[STREAM_HEADER_SIZE];
            const auto n = ::recv(fd, tmp, STREAM_HEADER_SIZE - s.header.size(), 0);
            if (n == 0 || (n < 0 && !would_block()))
                return close_stream(fd, false);
            if (n < 0)
                return;
            s.header.append(tmp, n);
            if (s.header.size() < STREAM_HEADER_SIZE)
                return;
            uint32_t dst_net;
            PortPair dst, src;
            std::memcpy(&dst_net, s.header.data(), 4);
            std::memcpy(&dst.port, s.header.data() + 4, 2);
            std::memcpy(&dst.vport, s.header.data() + 6, 2);
            std::memcpy(&src.port, s.header.data() + 8, 2);
            std::memcpy(&src.vport, s.header.data() + 10, 2);
            if (!src.port && !src.vport)
                src = { ADHOC_PORT, next_vport++ };
            if (next_vport < 0xC000)
                next_vport = 0xC000;
            s.peer = ntohl(dst_net);
            s.sid = next_sid++ & ~ACCEPTOR_BIT;
            s.header_done = true;
            stream_by_key[{ s.peer, s.sid, true }] = fd;
            std::string body;
            put32(body, s.peer);
            put32(body, s.sid);
            put16(body, src.port);
            put16(body, src.vport);
            put16(body, dst.port);
            put16(body, dst.vport);
            if (server_fd < 0)
                return close_stream(fd, false);
            RELAY_DEBUG("game opens a stream to " + ip_text(s.peer) + " " + pair_text(dst));
            send_frame(S_OPEN, body);
            return;
        }
        char tmp[16384];
        while (true) {
            const auto n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n > 0) {
                std::string body;
                body.reserve(8 + n);
                put32(body, s.peer);
                put32(body, s.sid | (s.initiator ? 0 : ACCEPTOR_BIT));
                body.append(tmp, n);
                send_frame(S_DATA, body);
                if (!streams.count(fd) || outbuf.size() > STREAM_READ_PAUSE)
                    return; // the connection dropped (closing every stream), or it is backed up
                continue;
            }
            if (n < 0 && would_block())
                return;
            return close_stream(fd, true); // the game closed its socket
        }
    }

    void flush_stream(int fd) {
        Stream &s = streams[fd];
        while (!s.out.empty()) {
            const auto n = ::send(fd, s.out.data(), s.out.size(), MSG_NOSIGNAL);
            if (n > 0) {
                s.out.erase(0, n);
                continue;
            }
            if (n < 0 && would_block())
                return;
            return close_stream(fd, true);
        }
        if (s.remote_closed)
            close_stream(fd, false);
    }

    // ---- main loop ----

    void loop() {
        std::vector<pollfd> fds;
        std::vector<int> stream_fds;
        while (running) {
            const int64_t now = now_ms();
            if (server_fd < 0 && now >= next_reconnect) {
                std::string error;
                server_fd = connect_server(5000, vip, error);
                if (server_fd < 0) {
                    RELAY_WARN("reconnect failed: " + error);
                    next_reconnect = now_ms() + 3000;
                } else {
                    RELAY_INFO("reconnected, virtual IP " + ip_text(vip));
                }
            }
            if (server_fd >= 0) {
                if (now - last_ping >= 5000) {
                    last_ping = now;
                    std::string body;
                    put64(body, static_cast<uint64_t>(now));
                    send_frame(PING, body);
                }
                if (server_fd >= 0 && now - last_recv > 30000)
                    drop_server("no answer from the relay server for 30 s");
            }

            fds.clear();
            stream_fds.clear();
            fds.push_back({ wake_fds[0], POLLIN, 0 });
            fds.push_back({ udp_fd, POLLIN, 0 });
            fds.push_back({ listen_fd, POLLIN, 0 });
            fds.push_back({ server_fd, static_cast<short>(server_fd >= 0 ? (POLLIN | (outbuf.empty() ? 0 : POLLOUT)) : 0), 0 });
            const bool read_streams = outbuf.size() <= STREAM_READ_PAUSE;
            for (const auto &[fd, s] : streams) {
                short events = 0;
                if (!s.header_done || (read_streams && !s.remote_closed))
                    events |= POLLIN;
                if (!s.out.empty())
                    events |= POLLOUT;
                fds.push_back({ fd, events, 0 });
                stream_fds.push_back(fd);
            }
            if (poll(fds.data(), fds.size(), server_fd < 0 ? 500 : 1000) < 0 && errno != EINTR) {
                RELAY_WARN("poll failed: " + std::string(strerror(errno)));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (fds[0].revents) {
                char drain[64];
                while (::read(wake_fds[0], drain, sizeof(drain)) > 0) {
                }
            }
            if (!running)
                break;
            if (fds[3].revents & (POLLIN | POLLHUP | POLLERR))
                read_server();
            if (server_fd >= 0 && (fds[3].revents & POLLOUT))
                flush_server();
            if (fds[1].revents & POLLIN)
                read_udp();
            if (fds[2].revents & POLLIN)
                accept_local();
            for (size_t i = 0; i < stream_fds.size(); i++) {
                const short ev = fds[4 + i].revents;
                const int fd = stream_fds[i];
                if (!ev || !streams.count(fd))
                    continue;
                if (ev & POLLOUT)
                    flush_stream(fd);
                if (streams.count(fd) && (ev & (POLLIN | POLLHUP | POLLERR)))
                    read_stream(fd);
            }
        }
        close_server();
    }
};

Link &link() {
    static Link instance;
    return instance;
}

} // namespace

bool start(const Config &config, int timeout_ms, std::string &error) { return link().start(config, timeout_ms, error); }

namespace {
std::atomic<bool> relay_wanted{ false };
} // namespace

void start_async(const Config &config) {
    std::thread([config] {
        std::string error;
        if (!link().start(config, 8000, error))
            RELAY_WARN(error);
    }).detach();
}
void set_wanted(bool wanted) { relay_wanted = wanted; }
bool wanted() { return relay_wanted; }
bool wait_active(int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!link().is_active() && relay_wanted && Clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return link().is_active();
}
void stop() { link().stop(); }
bool active() { return link().is_active(); }
uint32_t virtual_ip() { return link().virtual_ip(); }
uint32_t broadcast_ip() { return htonl(SUBNET | 0xFFFF); }
uint16_t bridge_udp_port() { return link().udp_port(); }
uint16_t bridge_tcp_port() { return link().tcp_port(); }
bool register_dgram(uint16_t local_port, uint16_t port, uint16_t vport) { return link().register_dgram(local_port, { port, vport }); }
bool register_listener(uint16_t local_port, uint16_t port, uint16_t vport) { return link().register_listener(local_port, { port, vport }); }
void unregister(uint16_t local_port) { link().unregister(local_port); }
bool lookup_accepted(uint16_t bridge_port, uint32_t &ip, uint16_t &port, uint16_t &vport) {
    Accepted a;
    if (!link().lookup_accepted(bridge_port, a))
        return false;
    ip = htonl(a.ip);
    port = a.pair.port;
    vport = a.pair.vport;
    return true;
}
std::string status() { return link().status(); }

} // namespace net_relay

#endif
