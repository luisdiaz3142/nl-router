#include "nl_router/metrics/exposer.hpp"
#include "nl_router/metrics/registry.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <poll.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

namespace nlr::metrics {

namespace {

// Read from fd until \r\n\r\n or limit. Returns the bytes read (request
// line + headers). Closes early on EOF or socket error. Cap at 8 KiB so a
// malicious peer can't tie us up streaming a huge "request".
std::string read_request_head(int fd) {
    constexpr std::size_t kMaxHead = 8 * 1024;
    std::string buf;
    buf.reserve(512);
    char tmp[1024];
    while (buf.size() < kMaxHead) {
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
        if (buf.find("\r\n\r\n") != std::string::npos) break;
    }
    return buf;
}

// Best-effort write all bytes. Returns true on success.
bool send_all(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

void write_response(int fd, int status, const std::string& reason,
                     const std::string& content_type, const std::string& body)
{
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    head += "Content-Type: " + content_type + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Connection: close\r\n\r\n";
    if (!send_all(fd, head)) return;
    if (!body.empty()) (void)send_all(fd, body);
}

}  // namespace

Exposer::Exposer(Registry& registry, std::uint16_t port, std::string bind_addr)
    : registry_(registry), port_(port), bind_addr_(std::move(bind_addr)) {}

Exposer::~Exposer() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void Exposer::start() {
    if (port_ == 0) return;       // disabled

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "{\"lvl\":\"warn\",\"msg\":\"metrics.exposer_socket_failed\","
                     "\"error\":\"" << std::strerror(errno) << "\"}\n";
        return;
    }
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    if (::inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr) != 1) {
        // Fall back to INADDR_ANY if the user passed a bad string.
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "{\"lvl\":\"warn\",\"msg\":\"metrics.exposer_bind_failed\","
                     "\"port\":" << port_ << ",\"error\":\""
                  << std::strerror(errno) << "\"}\n";
        ::close(listen_fd_); listen_fd_ = -1;
        return;
    }
    if (::listen(listen_fd_, 16) != 0) {
        std::cerr << "{\"lvl\":\"warn\",\"msg\":\"metrics.exposer_listen_failed\","
                     "\"error\":\"" << std::strerror(errno) << "\"}\n";
        ::close(listen_fd_); listen_fd_ = -1;
        return;
    }

    thread_ = std::thread([this] { accept_loop_(); });
    std::cerr << "{\"lvl\":\"info\",\"msg\":\"metrics.exposer_started\","
                 "\"port\":" << port_ << "}\n";
}

void Exposer::stop() noexcept {
    if (stop_requested_.exchange(true)) return;
    if (listen_fd_ >= 0) {
        // shutdown() unblocks accept() in the worker thread.
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void Exposer::accept_loop_() {
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // poll() with a short timeout so we can notice stop_requested_
        // even if no client connects.
        pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 500);
        if (pr <= 0) continue;     // timeout or signal — re-check stop
        if (!(pfd.revents & POLLIN)) continue;

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int cfd = ::accept(listen_fd_,
                                  reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            // Listening socket closed (stop_requested set). Exit.
            break;
        }

        const std::string head = read_request_head(cfd);

        // Parse the request line just enough to identify the path. We accept
        // any GET; anything else is 405.
        bool is_get = false;
        std::string path;
        const auto first_eol = head.find("\r\n");
        if (first_eol != std::string::npos) {
            const std::string line = head.substr(0, first_eol);
            // "GET /path HTTP/1.1"
            const auto s1 = line.find(' ');
            const auto s2 = (s1 == std::string::npos) ? std::string::npos
                                                       : line.find(' ', s1 + 1);
            if (s1 != std::string::npos && s2 != std::string::npos) {
                const std::string method = line.substr(0, s1);
                path = line.substr(s1 + 1, s2 - s1 - 1);
                is_get = (method == "GET");
            }
        }

        if (!is_get) {
            write_response(cfd, 405, "Method Not Allowed",
                            "text/plain; charset=utf-8",
                            "GET only\n");
        } else if (path == "/metrics") {
            write_response(cfd, 200, "OK",
                            "text/plain; version=0.0.4; charset=utf-8",
                            registry_.render());
        } else if (path == "/" || path == "/healthz") {
            // Lightweight health endpoint — useful for load balancers /
            // container readiness probes alongside the metrics port.
            write_response(cfd, 200, "OK",
                            "text/plain; charset=utf-8",
                            "ok\n");
        } else {
            write_response(cfd, 404, "Not Found",
                            "text/plain; charset=utf-8",
                            "not found\n");
        }
        ::close(cfd);
    }
}

}  // namespace nlr::metrics
