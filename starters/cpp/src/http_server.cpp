#include "http_server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace obsidio {
namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
constexpr std::size_t kReadChunk = 4096;
constexpr int kMaxEvents = 256;

const char* status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
  }
}

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------

struct Server::Connection {
  int fd = -1;
  std::string in;
  std::string out;
  std::size_t out_sent = 0;
  bool deferred = false;   // waiting on a worker; not in the epoll set
  bool close_after = false;
};

struct Server::Loop {
  std::size_t index = 0;
  int epoll_fd = -1;
  int listen_fd = -1;
  int wake_fd = -1;  // eventfd signalling deferred completions
  std::thread thread;
  std::unordered_map<int, std::unique_ptr<Connection>> connections;

  struct Completion {
    int fd;
    int status;
    std::string body;
    bool keep_alive;
  };
  std::mutex completions_mutex;
  std::deque<Completion> completions;
};

// ---------------------------------------------------------------------------

Server::Server(std::uint16_t port, std::size_t io_threads, Handler handler)
    : port_(port), io_threads_(io_threads), handler_(std::move(handler)) {}

Server::~Server() { stop(); }

void Server::build_response(std::string& out, int status, std::string_view body,
                            bool keep_alive) {
  char head[256];
  const int n = std::snprintf(
      head, sizeof(head),
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: %zu\r\n"
      "Connection: %s\r\n"
      "\r\n",
      status, status_text(status), body.size(),
      keep_alive ? "keep-alive" : "close");
  out.assign(head, static_cast<std::size_t>(n));
  out.append(body);
}

bool Server::bind_listener(Loop& loop) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket");
    return false;
  }

  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  // Each IO thread gets its own accept queue: the kernel spreads incoming
  // connections across them, so there is no shared accept lock.
#ifdef SO_REUSEPORT
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port_);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::perror("bind");
    ::close(fd);
    return false;
  }
  if (::listen(fd, 1024) != 0) {
    std::perror("listen");
    ::close(fd);
    return false;
  }
  if (!set_nonblocking(fd)) {
    ::close(fd);
    return false;
  }

  loop.listen_fd = fd;
  return true;
}

bool Server::start() {
  loops_.reserve(io_threads_);
  for (std::size_t i = 0; i < io_threads_; ++i) {
    auto loop = std::make_unique<Loop>();
    loop->index = i;

    loop->epoll_fd = ::epoll_create1(0);
    if (loop->epoll_fd < 0) {
      std::perror("epoll_create1");
      return false;
    }
    if (!bind_listener(*loop)) return false;

    loop->wake_fd = ::eventfd(0, EFD_NONBLOCK);
    if (loop->wake_fd < 0) {
      std::perror("eventfd");
      return false;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = loop->listen_fd;
    ::epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->listen_fd, &ev);

    ev.data.fd = loop->wake_fd;
    ::epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->wake_fd, &ev);

    loops_.push_back(std::move(loop));
  }
  return true;
}

void Server::run() {
  running_.store(true);
  for (std::size_t i = 1; i < loops_.size(); ++i) {
    loops_[i]->thread = std::thread([this, i] { loop_main(i); });
  }
  loop_main(0);  // run the first loop on the calling thread

  for (auto& loop : loops_) {
    if (loop->thread.joinable()) loop->thread.join();
  }
}

void Server::stop() {
  if (!running_.exchange(false)) return;
  // Nudge every loop so it notices running_ == false.
  for (auto& loop : loops_) {
    if (loop->wake_fd >= 0) {
      const std::uint64_t one = 1;
      [[maybe_unused]] ssize_t ignored = ::write(loop->wake_fd, &one, sizeof(one));
    }
  }
}

void Server::complete(const DeferContext& ctx, int status, std::string body) {
  Loop& loop = *loops_[static_cast<std::size_t>(ctx.loop_index)];
  {
    std::lock_guard<std::mutex> lock(loop.completions_mutex);
    loop.completions.push_back(
        Loop::Completion{ctx.fd, status, std::move(body), ctx.keep_alive});
  }
  const std::uint64_t one = 1;
  [[maybe_unused]] ssize_t ignored = ::write(loop.wake_fd, &one, sizeof(one));
}

// ---------------------------------------------------------------------------

void Server::loop_main(std::size_t index) {
  Loop& loop = *loops_[index];
  epoll_event events[kMaxEvents];

  auto close_connection = [&](int fd) {
    ::epoll_ctl(loop.epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    loop.connections.erase(fd);
    ::close(fd);
  };

  // Flush conn->out. Returns false if the connection died.
  auto flush = [&](Connection* conn) -> bool {
    while (conn->out_sent < conn->out.size()) {
      const ssize_t n = ::send(conn->fd, conn->out.data() + conn->out_sent,
                               conn->out.size() - conn->out_sent, MSG_NOSIGNAL);
      if (n > 0) {
        conn->out_sent += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = conn->fd;
        ::epoll_ctl(loop.epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
        return true;  // finish later
      }
      if (n < 0 && errno == EINTR) continue;
      return false;
    }

    conn->out.clear();
    conn->out_sent = 0;
    if (conn->close_after) return false;

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = conn->fd;
    ::epoll_ctl(loop.epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
    return true;
  };

  // Parse and dispatch as many complete requests as the buffer holds.
  auto process = [&](Connection* conn) -> bool {
    for (;;) {
      if (!conn->out.empty()) return true;  // still draining a response

      const std::size_t head_end = conn->in.find("\r\n\r\n");
      if (head_end == std::string::npos) {
        if (conn->in.size() > kMaxHeaderBytes) return false;
        return true;  // need more bytes
      }

      const std::string_view head(conn->in.data(), head_end);

      // Request line: METHOD SP TARGET SP VERSION
      const std::size_t sp1 = head.find(' ');
      if (sp1 == std::string_view::npos) return false;
      const std::size_t sp2 = head.find(' ', sp1 + 1);
      if (sp2 == std::string_view::npos) return false;

      Request req;
      req.method = head.substr(0, sp1);
      std::string_view target = head.substr(sp1 + 1, sp2 - sp1 - 1);

      const std::size_t qmark = target.find('?');
      if (qmark == std::string_view::npos) {
        req.path = target;
        req.query = {};
      } else {
        req.path = target.substr(0, qmark);
        req.query = target.substr(qmark + 1);
      }

      // HTTP/1.1 is keep-alive unless the client says otherwise.
      bool keep_alive = true;
      if (head.find("HTTP/1.0") != std::string_view::npos) keep_alive = false;
      for (const char* needle : {"Connection: close", "connection: close"}) {
        if (head.find(needle) != std::string_view::npos) keep_alive = false;
      }

      // Body, if any.
      std::size_t content_length = 0;
      for (const char* needle : {"Content-Length:", "content-length:"}) {
        const std::size_t pos = head.find(needle);
        if (pos != std::string_view::npos) {
          content_length = static_cast<std::size_t>(
              std::strtoul(conn->in.data() + pos + strlen(needle), nullptr, 10));
          break;
        }
      }

      const std::size_t body_start = head_end + 4;
      if (conn->in.size() < body_start + content_length) return true;  // need more
      req.body = std::string_view(conn->in.data() + body_start, content_length);

      DeferContext ctx{static_cast<int>(index), conn->fd, keep_alive};
      std::string body;
      int status = 200;
      const bool answered = handler_(req, ctx, body, status);

      const std::size_t consumed = body_start + content_length;
      conn->in.erase(0, consumed);

      if (!answered) {
        // A worker owns this connection now. Take it out of the epoll set so
        // this thread will not touch it until complete() hands it back.
        conn->deferred = true;
        ::epoll_ctl(loop.epoll_fd, EPOLL_CTL_DEL, conn->fd, nullptr);
        return true;
      }

      conn->close_after = !keep_alive;
      build_response(conn->out, status, body, keep_alive);
      conn->out_sent = 0;
      if (!flush(conn)) return false;
      if (conn->close_after) return false;
    }
  };

  auto accept_new = [&]() {
    for (;;) {
      const int fd = ::accept4(loop.listen_fd, nullptr, nullptr, SOCK_NONBLOCK);
      if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        return;
      }
      // Nagle would add tens of milliseconds to small responses. Off.
      int one = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

      auto conn = std::make_unique<Connection>();
      conn->fd = fd;
      conn->in.reserve(kReadChunk);

      epoll_event ev{};
      ev.events = EPOLLIN;
      ev.data.fd = fd;
      if (::epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        ::close(fd);
        continue;
      }
      loop.connections.emplace(fd, std::move(conn));
    }
  };

  auto drain_completions = [&]() {
    std::deque<Loop::Completion> batch;
    {
      std::lock_guard<std::mutex> lock(loop.completions_mutex);
      batch.swap(loop.completions);
    }
    for (auto& done : batch) {
      auto it = loop.connections.find(done.fd);
      if (it == loop.connections.end()) continue;  // already gone
      Connection* conn = it->second.get();

      conn->deferred = false;
      conn->close_after = !done.keep_alive;
      build_response(conn->out, done.status, done.body, done.keep_alive);
      conn->out_sent = 0;

      // Back into the epoll set before writing, so a partial write can arm
      // EPOLLOUT.
      epoll_event ev{};
      ev.events = EPOLLIN;
      ev.data.fd = conn->fd;
      ::epoll_ctl(loop.epoll_fd, EPOLL_CTL_ADD, conn->fd, &ev);

      if (!flush(conn) || conn->close_after) {
        close_connection(done.fd);
        continue;
      }
      if (!process(conn)) close_connection(done.fd);
    }
  };

  while (running_.load(std::memory_order_relaxed)) {
    const int n = ::epoll_wait(loop.epoll_fd, events, kMaxEvents, 200);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;

      if (fd == loop.listen_fd) {
        accept_new();
        continue;
      }

      if (fd == loop.wake_fd) {
        std::uint64_t counter = 0;
        while (::read(loop.wake_fd, &counter, sizeof(counter)) > 0) {
        }
        drain_completions();
        continue;
      }

      auto it = loop.connections.find(fd);
      if (it == loop.connections.end()) continue;
      Connection* conn = it->second.get();

      if (events[i].events & (EPOLLHUP | EPOLLERR)) {
        if (!conn->deferred) close_connection(fd);
        continue;
      }

      if (events[i].events & EPOLLOUT) {
        if (!flush(conn) || conn->close_after) {
          close_connection(fd);
          continue;
        }
      }

      if (events[i].events & EPOLLIN) {
        bool alive = true;
        for (;;) {
          char buf[kReadChunk];
          const ssize_t got = ::recv(conn->fd, buf, sizeof(buf), 0);
          if (got > 0) {
            conn->in.append(buf, static_cast<std::size_t>(got));
            if (static_cast<std::size_t>(got) < sizeof(buf)) break;
            continue;
          }
          if (got == 0) { alive = false; break; }
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          if (errno == EINTR) continue;
          alive = false;
          break;
        }
        if (alive) alive = process(conn);
        if (!alive && !conn->deferred) {
          close_connection(fd);
          continue;
        }
      }
    }
  }

  // Teardown: deferred connections are closed by the worker's completion path
  // being dropped, which is fine at shutdown.
  for (auto& [fd, conn] : loop.connections) ::close(fd);
  loop.connections.clear();
  if (loop.listen_fd >= 0) ::close(loop.listen_fd);
  if (loop.wake_fd >= 0) ::close(loop.wake_fd);
  if (loop.epoll_fd >= 0) ::close(loop.epoll_fd);
}

}  // namespace obsidio
