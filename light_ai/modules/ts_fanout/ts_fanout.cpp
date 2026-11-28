#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kChunkSize = 16384;
constexpr std::size_t kClientBufSize = 262144;  // 256 KiB per-client isolation buffer
constexpr std::size_t kTsPacketSize = 188;
constexpr uint8_t kTsSyncByte = 0x47;
constexpr int kSendBufSize = 65536;
constexpr int kNotsentLowat = 16384;
constexpr int kRetryIntervalMs = 250;
constexpr int kAcceptBackoffMs = 100;  // listener parked after fd / buffer exhaustion in accept4()
constexpr int kMaxDroppedInRow = 16;   // failed pending connections in a row before accept4() yields to epoll
constexpr int64_t kConnectTimeoutMs = 3000;  // upstream connect() still in EINPROGRESS: give up, retry
constexpr int kMaxEpollEvents = 32;
constexpr int kMaxReadIters = 8;

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) noexcept {
    g_stop = 1;
}

int64_t now_ms() noexcept {
    const auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp).count();
}

struct Endpoint {
    std::string host;
    uint16_t port = 0;
    sockaddr_in addr{};
};

bool parse_endpoint(const char* text, Endpoint& out) noexcept {
    if (text == nullptr) {
        return false;
    }
    const std::string s(text);
    const std::size_t colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
        return false;
    }
    const std::string host = s.substr(0, colon);
    const std::string port_str = s.substr(colon + 1);
    char* end = nullptr;
    const long port_val = std::strtol(port_str.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || port_val <= 0 || port_val > 65535) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_val));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return false;
    }
    out.host = host;
    out.port = static_cast<uint16_t>(port_val);
    out.addr = addr;
    return true;
}

struct Config {
    Endpoint upstream{};
    Endpoint listen{};
    std::size_t max_clients = 8;
    int64_t idle_grace_ms = 1500;
};

void print_usage(const char* prog) noexcept {
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --upstream <ip:port>     Upstream TCP MPEG-TS source (default 127.0.0.1:1234)\n"
                 "  --listen <ip:port>       Listen address for clients (default 0.0.0.0:1236)\n"
                 "  --max-clients <N>        Maximum concurrent clients (default 8)\n"
                 "  --idle-grace-ms <ms>     Keep upstream open after last client (default 1500)\n"
                 "  -h, --help               Show this help\n",
                 prog);
}

bool parse_args(int argc, char** argv, Config& cfg) noexcept {
    if (!parse_endpoint("127.0.0.1:1234", cfg.upstream) ||
        !parse_endpoint("0.0.0.0:1236", cfg.listen)) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "--upstream" && i + 1 < argc) {
            if (!parse_endpoint(argv[++i], cfg.upstream)) {
                std::fprintf(stderr, "[ts_fanout] invalid --upstream: %s\n", argv[i]);
                return false;
            }
        } else if (arg == "--listen" && i + 1 < argc) {
            if (!parse_endpoint(argv[++i], cfg.listen)) {
                std::fprintf(stderr, "[ts_fanout] invalid --listen: %s\n", argv[i]);
                return false;
            }
        } else if (arg == "--max-clients" && i + 1 < argc) {
            char* end = nullptr;
            const long v = std::strtol(argv[++i], &end, 10);
            if (end == nullptr || *end != '\0' || v <= 0 || v > 1024) {
                std::fprintf(stderr, "[ts_fanout] invalid --max-clients: %s\n", argv[i]);
                return false;
            }
            cfg.max_clients = static_cast<std::size_t>(v);
        } else if (arg == "--idle-grace-ms" && i + 1 < argc) {
            char* end = nullptr;
            const long long v = std::strtoll(argv[++i], &end, 10);
            if (end == nullptr || *end != '\0' || v < 0 || v > 600000) {
                std::fprintf(stderr, "[ts_fanout] invalid --idle-grace-ms: %s\n", argv[i]);
                return false;
            }
            cfg.idle_grace_ms = static_cast<int64_t>(v);
        } else {
            std::fprintf(stderr, "[ts_fanout] unknown or incomplete option: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

// The upstream socket only receives: no send-side tuning.
void configure_upstream_tcp(int fd) noexcept {
    const int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// A viewer socket: a short kernel send queue, so a slow viewer shows up in its own ring.
void configure_client_tcp(int fd) noexcept {
    const int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    const int sndbuf = kSendBufSize;
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
#ifdef TCP_NOTSENT_LOWAT
    const int lowat = kNotsentLowat;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
}

bool epoll_add(int epfd, int fd, uint32_t events, uint64_t token) noexcept {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = token;
    return ::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool epoll_mod(int epfd, int fd, uint32_t events, uint64_t token) noexcept {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = token;
    return ::epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void epoll_del_close(int epfd, int& fd) noexcept {
    if (fd >= 0) {
        (void)::epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        fd = -1;
    }
}

// One slot of the fixed pool: allocated once, reused in place (the 256 KiB ring never moves).
struct Client {
    bool active = false;
    uint64_t id = 0;
    int fd = -1;
    bool synced = false;
    bool epoll_out = false;
    std::size_t head = 0;
    std::size_t len = 0;
    std::size_t sync_len = 0;
    std::array<uint8_t, 4 * kTsPacketSize> sync_buf{};
    std::array<uint8_t, kClientBufSize> buf{};

    bool push_bytes(const uint8_t* data, std::size_t n) noexcept {
        if (n > kClientBufSize - len) {
            return false;
        }
        const std::size_t tail = (head + len) % kClientBufSize;
        const std::size_t first = std::min(n, kClientBufSize - tail);
        std::memcpy(buf.data() + tail, data, first);
        if (n > first) {
            std::memcpy(buf.data(), data + first, n - first);
        }
        len += n;
        return true;
    }
};

// Returns false if client had a fatal send error and must be dropped.
bool flush_client(Client& c) noexcept {
    while (c.len > 0) {
        const std::size_t contig = std::min(c.len, kClientBufSize - c.head);
        const ssize_t sent = ::send(c.fd, c.buf.data() + c.head, contig, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0) {
            const std::size_t usent = static_cast<std::size_t>(sent);
            c.head = (c.head + usent) % kClientBufSize;
            c.len -= usent;
            if (c.len == 0) {
                c.head = 0;
            }
            if (usent < contig) {
                return true;
            }
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// Fast-path send when buffer is empty, queuing only unsent remainder.
bool enqueue_client_data(Client& c, const uint8_t* data, std::size_t n) noexcept {
    if (n == 0) {
        return true;
    }
    std::size_t offset = 0;
    if (c.len == 0) {
        while (offset < n) {
            const ssize_t sent = ::send(c.fd, data + offset, n - offset, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
            } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
    }
    if (offset < n) {
        return c.push_bytes(data + offset, n - offset);
    }
    return true;
}

std::size_t find_ts_sync(const uint8_t* data, std::size_t n) noexcept {
    if (n > 2 * kTsPacketSize) {
        const std::size_t limit3 = n - 2 * kTsPacketSize;
        for (std::size_t i = 0; i < limit3; ++i) {
            if (data[i] == kTsSyncByte &&
                data[i + kTsPacketSize] == kTsSyncByte &&
                data[i + 2 * kTsPacketSize] == kTsSyncByte) {
                return i;
            }
        }
    }
    return n;
}

bool feed_client_sync(Client& c, const uint8_t* data, std::size_t n) noexcept {
    if (c.sync_len == 0 && n > 2 * kTsPacketSize) {
        const std::size_t off = find_ts_sync(data, n);
        if (off < n) {
            c.synced = true;
            return enqueue_client_data(c, data + off, n - off);
        }
    }
    // Accumulate small or unaligned chunks until 3 consecutive 0x47 sync bytes, 188 B apart, appear.
    const std::size_t cap = c.sync_buf.size();
    std::size_t pos = 0;
    while (pos < n) {
        const std::size_t take = std::min(n - pos, cap - c.sync_len);
        std::memcpy(c.sync_buf.data() + c.sync_len, data + pos, take);
        c.sync_len += take;
        pos += take;
        const std::size_t off = find_ts_sync(c.sync_buf.data(), c.sync_len);
        if (off < c.sync_len) {
            c.synced = true;
            const bool ok1 = enqueue_client_data(c, c.sync_buf.data() + off, c.sync_len - off);
            c.sync_len = 0;
            if (!ok1) {
                return false;
            }
            if (pos < n) {
                return enqueue_client_data(c, data + pos, n - pos);
            }
            return true;
        }
        if (c.sync_len == cap) {
            // Keep trailing 2 * kTsPacketSize bytes for cross-chunk sync detection.
            const std::size_t keep = 2 * kTsPacketSize;
            std::memmove(c.sync_buf.data(), c.sync_buf.data() + (cap - keep), keep);
            c.sync_len = keep;
        }
    }
    return true;
}

// deadline < 0: none yet.
int64_t earlier_deadline(int64_t deadline, int64_t candidate) noexcept {
    return (deadline < 0) ? candidate : std::min(deadline, candidate);
}

enum class UpstreamState {
    Disconnected,
    Connecting,
    Connected,
};

constexpr uint64_t kTokenListen = 1;

struct Relay {
    Config cfg{};
    sigset_t epoll_sigmask{};
    int epfd = -1;
    int listen_fd = -1;
    int upstream_fd = -1;
    uint64_t upstream_token = 0;
    uint64_t next_token = 2;
    UpstreamState up_state = UpstreamState::Disconnected;
    int64_t next_retry_ms = 0;
    int64_t idle_deadline_ms = 0;
    int64_t connect_deadline_ms = 0;  // set while Connecting
    int64_t last_connect_err_log_ms = 0;
    bool listen_armed = false;  // listen_fd is registered in epfd
    int64_t accept_resume_ms = 0;
    std::vector<Client> clients{};  // cfg.max_clients slots, sized once in main()
    std::size_t active_clients = 0;

    // Puts a parked listener back into epfd; on failure tries again after the backoff.
    void try_rearm_listen(int64_t now) noexcept {
        if (listen_armed || listen_fd < 0) {
            return;
        }
        if (epoll_add(epfd, listen_fd, EPOLLIN, kTokenListen)) {
            listen_armed = true;
            accept_resume_ms = 0;
            return;
        }
        accept_resume_ms = now + kAcceptBackoffMs;
    }

    void update_client_epoll(Client& c) noexcept {
        const bool want_out = (c.len > 0);
        if (want_out == c.epoll_out) {
            return;
        }
        const uint32_t ev = EPOLLIN | EPOLLRDHUP | (want_out ? EPOLLOUT : 0U);
        if (epoll_mod(epfd, c.fd, ev, c.id)) {
            c.epoll_out = want_out;
        }
    }

    void drop_client_at(std::size_t idx, const char* reason) noexcept {
        if (idx >= clients.size() || !clients[idx].active) {
            return;
        }
        Client& c = clients[idx];
        const int fd = c.fd;
        epoll_del_close(epfd, c.fd);
        c.active = false;
        c.id = 0;
        c.fd = -1;
        c.len = 0;
        c.head = 0;
        c.synced = false;
        c.sync_len = 0;
        c.epoll_out = false;
        --active_clients;
        std::fprintf(stderr, "[ts_fanout] client fd=%d disconnected (%s), active=%zu\n",
                     fd, reason, active_clients);
        try_rearm_listen(now_ms());  // a descriptor is free again
        if (active_clients == 0 && up_state != UpstreamState::Disconnected) {
            idle_deadline_ms = now_ms() + cfg.idle_grace_ms;
        }
    }

    void close_upstream(const char* reason, bool quiet = false) noexcept {
        if (upstream_fd >= 0) {
            epoll_del_close(epfd, upstream_fd);
            if (!quiet) {
                std::fprintf(stderr, "[ts_fanout] upstream closed (%s)\n", reason);
            }
            try_rearm_listen(now_ms());  // a descriptor is free again
        }
        up_state = UpstreamState::Disconnected;
        idle_deadline_ms = 0;
        connect_deadline_ms = 0;
        // The next upstream session starts mid-packet: drop what is still queued and realign every client.
        for (auto& c : clients) {
            if (!c.active) {
                continue;
            }
            c.head = 0;
            c.len = 0;
            c.synced = false;
            c.sync_len = 0;
            update_client_epoll(c);
        }
    }

    void start_upstream_connect(int64_t now) noexcept {
        if (up_state != UpstreamState::Disconnected) {
            return;
        }
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            next_retry_ms = now + kRetryIntervalMs;
            return;
        }
        configure_upstream_tcp(fd);
        const uint64_t token = next_token++;
        const int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&cfg.upstream.addr),
                                 sizeof(cfg.upstream.addr));
        if (rc == 0) {
            if (!epoll_add(epfd, fd, EPOLLIN | EPOLLRDHUP, token)) {
                ::close(fd);
                next_retry_ms = now + kRetryIntervalMs;
                return;
            }
            upstream_fd = fd;
            upstream_token = token;
            up_state = UpstreamState::Connected;
            connect_deadline_ms = 0;
            std::fprintf(stderr, "[ts_fanout] upstream connected to %s:%u\n",
                         cfg.upstream.host.c_str(), cfg.upstream.port);
            return;
        }
        if (errno == EINPROGRESS) {
            if (!epoll_add(epfd, fd, EPOLLOUT | EPOLLRDHUP, token)) {
                ::close(fd);
                next_retry_ms = now + kRetryIntervalMs;
                return;
            }
            upstream_fd = fd;
            upstream_token = token;
            up_state = UpstreamState::Connecting;
            connect_deadline_ms = now + kConnectTimeoutMs;
            return;
        }
        ::close(fd);
        next_retry_ms = now + kRetryIntervalMs;
    }

    // The first inactive slot, nullptr when the pool is full.
    Client* free_slot() noexcept {
        for (auto& c : clients) {
            if (!c.active) {
                return &c;
            }
        }
        return nullptr;
    }

    // accept4() errors that belong to the one pending connection: ECONNABORTED and the network errors that Linux
    // passes through from the new socket (accept(2), "Error handling").
    static bool is_dropped_connection_error(int err) noexcept {
        switch (err) {
        case ECONNABORTED:
        case ENETDOWN:
        case EPROTO:
        case ENOPROTOOPT:
        case EHOSTDOWN:
        case ENONET:
        case EHOSTUNREACH:
        case EOPNOTSUPP:
        case ENETUNREACH:
            return true;
        default:
            return false;
        }
    }

    void handle_accept() noexcept {
        int dropped_in_row = 0;
        while (listen_armed) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const int cfd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd < 0) {
                const int err = errno;
                if (err == EINTR) {
                    continue;
                }
                // This connection is gone, the queue may hold more (accept(2)). Bounded: an error that stays (EOPNOTSUPP
                // can also name the listener itself) must not keep the loop away from the other descriptors.
                if (is_dropped_connection_error(err) && ++dropped_in_row < kMaxDroppedInRow) {
                    continue;
                }
                if (err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM) {
                    // The connection stays queued: a level-triggered listen_fd left in epfd would busy-spin.
                    std::fprintf(stderr, "[ts_fanout] accept failed (%s), listener paused for %d ms\n",
                                 std::strerror(err), kAcceptBackoffMs);
                    (void)::epoll_ctl(epfd, EPOLL_CTL_DEL, listen_fd, nullptr);
                    listen_armed = false;
                    accept_resume_ms = now_ms() + kAcceptBackoffMs;
                }
                break;  // EAGAIN: the queue is drained; any other error: back to epoll
            }
            dropped_in_row = 0;
            if (active_clients >= cfg.max_clients) {
                std::fprintf(stderr, "[ts_fanout] max clients (%zu) reached, rejecting fd=%d\n",
                             cfg.max_clients, cfd);
                ::close(cfd);
                continue;
            }
            Client* const slot = free_slot();
            if (slot == nullptr) {
                ::close(cfd);
                continue;
            }
            configure_client_tcp(cfd);
            const uint64_t cid = next_token++;
            if (!epoll_add(epfd, cfd, EPOLLIN | EPOLLRDHUP, cid)) {
                ::close(cfd);
                continue;
            }
            slot->active = true;
            slot->id = cid;
            slot->fd = cfd;
            slot->synced = false;
            slot->epoll_out = false;
            slot->head = 0;
            slot->len = 0;
            slot->sync_len = 0;
            ++active_clients;
            idle_deadline_ms = 0;

            char ip_buf[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &peer.sin_addr, ip_buf, sizeof(ip_buf));
            std::fprintf(stderr, "[ts_fanout] client fd=%d connected from %s:%u, active=%zu\n",
                         cfd, ip_buf, ntohs(peer.sin_port), active_clients);

            if (up_state == UpstreamState::Disconnected) {
                start_upstream_connect(now_ms());
            }
        }
    }

    void handle_upstream_event(uint32_t events, int64_t now) noexcept {
        if (up_state == UpstreamState::Connecting) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(upstream_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                err = errno;
            }
            if ((events & (EPOLLERR | EPOLLHUP)) != 0 && err == 0) {
                err = ECONNREFUSED;
            }
            if (err != 0) {
                const bool should_log = (now - last_connect_err_log_ms >= 5000);
                if (should_log) {
                    last_connect_err_log_ms = now;
                    std::fprintf(stderr, "[ts_fanout] upstream connect failed (%s)\n", std::strerror(err));
                }
                close_upstream("connect failed", true);
                next_retry_ms = now + kRetryIntervalMs;
                return;
            }
            if (!epoll_mod(epfd, upstream_fd, EPOLLIN | EPOLLRDHUP, upstream_token)) {
                close_upstream("epoll_mod failed");
                next_retry_ms = now + kRetryIntervalMs;
                return;
            }
            up_state = UpstreamState::Connected;
            connect_deadline_ms = 0;
            last_connect_err_log_ms = 0;
            std::fprintf(stderr, "[ts_fanout] upstream connected to %s:%u\n",
                         cfg.upstream.host.c_str(), cfg.upstream.port);
            return;
        }

        if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0 && (events & EPOLLIN) == 0) {
            close_upstream("peer closed");
            next_retry_ms = now + kRetryIntervalMs;
            return;
        }

        std::array<uint8_t, kChunkSize> chunk{};
        for (int iter = 0; iter < kMaxReadIters; ++iter) {
            const ssize_t n = ::recv(upstream_fd, chunk.data(), chunk.size(), MSG_DONTWAIT);
            if (n > 0) {
                if (active_clients == 0) {
                    continue;  // Drain upstream during idle grace without blocking
                }
                const std::size_t un = static_cast<std::size_t>(n);
                for (std::size_t i = 0; i < clients.size(); ++i) {
                    Client& c = clients[i];
                    if (!c.active) {
                        continue;
                    }
                    const bool ok = c.synced
                        ? enqueue_client_data(c, chunk.data(), un)
                        : feed_client_sync(c, chunk.data(), un);
                    if (!ok) {
                        drop_client_at(i, "slow consumer or send error");
                        continue;
                    }
                    update_client_epoll(c);
                }
            } else if (n == 0) {
                close_upstream("EOF");
                next_retry_ms = now + kRetryIntervalMs;
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                close_upstream(std::strerror(errno));
                next_retry_ms = now + kRetryIntervalMs;
                return;
            }
        }
    }

    void handle_client_event(uint64_t id, uint32_t events) noexcept {
        std::size_t idx = clients.size();
        for (std::size_t i = 0; i < clients.size(); ++i) {
            if (clients[i].active && clients[i].id == id) {
                idx = i;
                break;
            }
        }
        if (idx == clients.size()) {
            return;
        }
        const int fd = clients[idx].fd;
        if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
            drop_client_at(idx, "peer closed");
            return;
        }
        if ((events & EPOLLIN) != 0) {
            std::array<uint8_t, 512> discard{};
            const ssize_t r = ::recv(fd, discard.data(), discard.size(), MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                drop_client_at(idx, "read EOF/error");
                return;
            }
        }
        if ((events & EPOLLOUT) != 0) {
            if (!flush_client(clients[idx])) {
                drop_client_at(idx, "write error");
                return;
            }
            update_client_epoll(clients[idx]);
        }
    }

    // The epoll wait up to the earliest active deadline, -1 = none. They are independent: with no
    // client left during a connect, the idle grace and the connect timeout both run.
    int compute_timeout_ms(int64_t now) const noexcept {
        int64_t deadline = -1;
        if (active_clients > 0 && up_state == UpstreamState::Disconnected) {
            deadline = earlier_deadline(deadline, next_retry_ms);
        }
        if (active_clients == 0 && up_state != UpstreamState::Disconnected && idle_deadline_ms > 0) {
            deadline = earlier_deadline(deadline, idle_deadline_ms);
        }
        if (up_state == UpstreamState::Connecting && connect_deadline_ms > 0) {
            deadline = earlier_deadline(deadline, connect_deadline_ms);
        }
        if (!listen_armed && accept_resume_ms > 0) {
            deadline = earlier_deadline(deadline, accept_resume_ms);
        }
        if (deadline < 0) {
            return -1;
        }
        return static_cast<int>(std::max<int64_t>(0, deadline - now));
    }

    int run() noexcept {
        epfd = ::epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) {
            std::perror("[ts_fanout] epoll_create1");
            return 1;
        }
        listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) {
            std::perror("[ts_fanout] socket");
            ::close(epfd);
            return 1;
        }
        const int one = 1;
        (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&cfg.listen.addr),
                   sizeof(cfg.listen.addr)) < 0) {
            std::perror("[ts_fanout] bind");
            ::close(listen_fd);
            ::close(epfd);
            return 1;
        }
        if (::listen(listen_fd, 16) < 0) {
            std::perror("[ts_fanout] listen");
            ::close(listen_fd);
            ::close(epfd);
            return 1;
        }
        if (!epoll_add(epfd, listen_fd, EPOLLIN, kTokenListen)) {
            std::perror("[ts_fanout] epoll_add listen");
            ::close(listen_fd);
            ::close(epfd);
            return 1;
        }
        listen_armed = true;

        std::fprintf(stderr,
                     "[ts_fanout] listening on %s:%u -> upstream %s:%u (max_clients=%zu, idle_grace=%lldms)\n",
                     cfg.listen.host.c_str(), cfg.listen.port,
                     cfg.upstream.host.c_str(), cfg.upstream.port,
                     cfg.max_clients, static_cast<long long>(cfg.idle_grace_ms));

        std::array<epoll_event, kMaxEpollEvents> events{};
        while (g_stop == 0) {
            const int64_t now = now_ms();
            // Before the retry branch: a timed-out attempt waits kRetryIntervalMs like any failed one.
            if (up_state == UpstreamState::Connecting && connect_deadline_ms > 0 && now >= connect_deadline_ms) {
                close_upstream("connect timeout");
                next_retry_ms = now + kRetryIntervalMs;
            }
            if (active_clients > 0 && up_state == UpstreamState::Disconnected && now >= next_retry_ms) {
                start_upstream_connect(now);
            }
            if (active_clients == 0 && up_state != UpstreamState::Disconnected &&
                idle_deadline_ms > 0 && now >= idle_deadline_ms) {
                close_upstream("idle grace expired");
            }
            if (!listen_armed && now >= accept_resume_ms) {
                try_rearm_listen(now);
            }

            const int timeout_ms = compute_timeout_ms(now_ms());
            const int nev = ::epoll_pwait(epfd, events.data(), kMaxEpollEvents, timeout_ms, &epoll_sigmask);
            if (nev < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::perror("[ts_fanout] epoll_pwait");
                break;
            }
            const int64_t ev_now = now_ms();
            for (int i = 0; i < nev; ++i) {
                const uint64_t token = events[static_cast<std::size_t>(i)].data.u64;
                const uint32_t ev = events[static_cast<std::size_t>(i)].events;
                if (token == kTokenListen) {
                    handle_accept();
                } else if (upstream_fd >= 0 && token == upstream_token) {
                    handle_upstream_event(ev, ev_now);
                } else {
                    handle_client_event(token, ev);
                }
            }
        }

        std::fprintf(stderr, "[ts_fanout] shutting down\n");
        for (std::size_t i = 0; i < clients.size(); ++i) {
            if (clients[i].active) {
                epoll_del_close(epfd, clients[i].fd);
                clients[i].active = false;
                --active_clients;
            }
        }
        close_upstream("shutdown");
        epoll_del_close(epfd, listen_fd);
        ::close(epfd);
        return 0;
    }
};

}  // namespace

int main(int argc, char** argv) {
    struct sigaction sa_ign{};
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    static_cast<void>(::sigaction(SIGPIPE, &sa_ign, nullptr));

    struct sigaction sa_stop{};
    sa_stop.sa_handler = handle_signal;
    sigemptyset(&sa_stop.sa_mask);
    sa_stop.sa_flags = 0;
    static_cast<void>(::sigaction(SIGINT, &sa_stop, nullptr));
    static_cast<void>(::sigaction(SIGTERM, &sa_stop, nullptr));

    sigset_t block_mask{};
    sigemptyset(&block_mask);
    sigaddset(&block_mask, SIGINT);
    sigaddset(&block_mask, SIGTERM);

    Relay relay{};
    if (::sigprocmask(SIG_BLOCK, &block_mask, &relay.epoll_sigmask) != 0) {
        std::perror("[ts_fanout] sigprocmask");
        return 1;
    }
    if (!parse_args(argc, argv, relay.cfg)) {
        return 1;
    }
    relay.clients.resize(relay.cfg.max_clients);
    return relay.run();
}
