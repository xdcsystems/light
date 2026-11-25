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

void configure_low_latency_tcp(int fd) noexcept {
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

struct Client {
    uint64_t id = 0;
    int fd = -1;
    bool synced = false;
    bool epoll_out = false;
    std::size_t head = 0;
    std::size_t len = 0;
    std::size_t sync_len = 0;
    std::array<uint8_t, 3 * kTsPacketSize> sync_buf{};
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
    if (n > kTsPacketSize) {
        const std::size_t limit2 = n - kTsPacketSize;
        for (std::size_t i = 0; i < limit2; ++i) {
            if (data[i] == kTsSyncByte && data[i + kTsPacketSize] == kTsSyncByte) {
                return i;
            }
        }
    }
    return n;
}

bool feed_client_sync(Client& c, const uint8_t* data, std::size_t n) noexcept {
    if (c.sync_len == 0 && n > kTsPacketSize) {
        const std::size_t off = find_ts_sync(data, n);
        if (off < n) {
            c.synced = true;
            return enqueue_client_data(c, data + off, n - off);
        }
    }
    // Accumulate small or unaligned chunks until >= 2 consecutive 0x47 sync bytes appear.
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
    int64_t last_connect_err_log_ms = 0;
    std::vector<Client> clients{};

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
        const int fd = clients[idx].fd;
        epoll_del_close(epfd, clients[idx].fd);
        if (idx + 1 < clients.size()) {
            clients[idx] = std::move(clients.back());
        }
        clients.pop_back();
        std::fprintf(stderr, "[ts_fanout] client fd=%d disconnected (%s), active=%zu\n",
                     fd, reason, clients.size());
        if (clients.empty() && up_state != UpstreamState::Disconnected) {
            idle_deadline_ms = now_ms() + cfg.idle_grace_ms;
        }
    }

    void close_upstream(const char* reason, bool quiet = false) noexcept {
        if (upstream_fd >= 0) {
            epoll_del_close(epfd, upstream_fd);
            if (!quiet) {
                std::fprintf(stderr, "[ts_fanout] upstream closed (%s)\n", reason);
            }
        }
        up_state = UpstreamState::Disconnected;
        idle_deadline_ms = 0;
        for (auto& c : clients) {
            c.synced = false;
            c.sync_len = 0;
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
        configure_low_latency_tcp(fd);
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
            return;
        }
        ::close(fd);
        next_retry_ms = now + kRetryIntervalMs;
    }

    void handle_accept() noexcept {
        for (;;) {
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            const int cfd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len,
                                      SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (clients.size() >= cfg.max_clients) {
                std::fprintf(stderr, "[ts_fanout] max clients (%zu) reached, rejecting fd=%d\n",
                             cfg.max_clients, cfd);
                ::close(cfd);
                continue;
            }
            configure_low_latency_tcp(cfd);
            const uint64_t cid = next_token++;
            if (!epoll_add(epfd, cfd, EPOLLIN | EPOLLRDHUP, cid)) {
                ::close(cfd);
                continue;
            }
            clients.emplace_back();
            clients.back().id = cid;
            clients.back().fd = cfd;
            idle_deadline_ms = 0;

            char ip_buf[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &peer.sin_addr, ip_buf, sizeof(ip_buf));
            std::fprintf(stderr, "[ts_fanout] client fd=%d connected from %s:%u, active=%zu\n",
                         cfd, ip_buf, ntohs(peer.sin_port), clients.size());

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
                if (clients.empty()) {
                    continue;  // Drain upstream during idle grace without blocking
                }
                const std::size_t un = static_cast<std::size_t>(n);
                for (std::size_t i = 0; i < clients.size();) {
                    Client& c = clients[i];
                    const bool ok = c.synced
                        ? enqueue_client_data(c, chunk.data(), un)
                        : feed_client_sync(c, chunk.data(), un);
                    if (!ok) {
                        drop_client_at(i, "slow consumer or send error");
                        continue;
                    }
                    update_client_epoll(c);
                    ++i;
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
            if (clients[i].id == id) {
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

    int compute_timeout_ms(int64_t now) const noexcept {
        int64_t deadline = -1;
        if (!clients.empty() && up_state == UpstreamState::Disconnected) {
            deadline = next_retry_ms;
        } else if (clients.empty() && up_state != UpstreamState::Disconnected && idle_deadline_ms > 0) {
            deadline = idle_deadline_ms;
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

        std::fprintf(stderr,
                     "[ts_fanout] listening on %s:%u -> upstream %s:%u (max_clients=%zu, idle_grace=%lldms)\n",
                     cfg.listen.host.c_str(), cfg.listen.port,
                     cfg.upstream.host.c_str(), cfg.upstream.port,
                     cfg.max_clients, static_cast<long long>(cfg.idle_grace_ms));

        std::array<epoll_event, kMaxEpollEvents> events{};
        while (g_stop == 0) {
            const int64_t now = now_ms();
            if (!clients.empty() && up_state == UpstreamState::Disconnected && now >= next_retry_ms) {
                start_upstream_connect(now);
            }
            if (clients.empty() && up_state != UpstreamState::Disconnected &&
                idle_deadline_ms > 0 && now >= idle_deadline_ms) {
                close_upstream("idle grace expired");
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
        while (!clients.empty()) {
            epoll_del_close(epfd, clients.back().fd);
            clients.pop_back();
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
    relay.clients.reserve(relay.cfg.max_clients);
    return relay.run();
}
