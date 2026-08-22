#include "http_server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#if (__linux__)
  #include <sys/epoll.h>
  #include <sys/eventfd.h>
#endif

#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace obsidio {
namespace {

constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
// The only endpoint with a body is POST /price and its JSON is ~50 bytes, so
// this is three orders of magnitude of headroom. Anything larger is not a
// client of this API; buffering it would only donate our 2 GB cap to whoever
// is sending it.
constexpr std::size_t kMaxBodyBytes = 64 * 1024;
constexpr std::size_t kReadChunk = 4096;
constexpr int kMaxEvents = 256;

const char* status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
  }
}

inline char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
  }
  return true;
}

bool icontains(std::string_view hay, std::string_view needle) {
  if (needle.empty() || hay.size() < needle.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    if (iequals(hay.substr(i, needle.size()), needle)) return true;
  }
  return false;
}

// See the use site in accept_new(): a test-only SO_SNDBUF pin, 0 when unset.
const int forced_sndbuf = [] {
  const char* raw = std::getenv("OBSIDIO_SNDBUF");
  return (raw != nullptr && *raw != '\0') ? std::atoi(raw) : 0;
}();

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
  // Torn down, but not yet reclaimed -- see close_connection().
  bool closed = false;
  // The event mask currently registered with epoll for this fd, or 0 when the
  // fd is not in the set at all. Lets arm() skip epoll_ctl entirely when the
  // interest set already is what we want, which is every successful flush on a
  // keep-alive connection: one of the ~3 syscalls per request, and the
  // response path is 60% of the scored weight.
  std::uint32_t armed = 0;
};

struct Server::Loop {
  std::size_t index = 0;
  int epoll_fd = -1;
  int listen_fd = -1;
  int wake_fd = -1;  // eventfd signalling deferred completions
  std::thread thread;
  std::unordered_map<int, std::unique_ptr<Connection>> connections;

  // Every connection event carries its Connection* in epoll_event::data.ptr,
  // so the two fds that have no Connection need some other identity. Their
  // events carry the address of one of these instead; both are compared by
  // address only, and the values are never read.
  char listen_tag = 0;
  char wake_tag = 0;

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

    // The Loop itself is heap-allocated and never moves, so the addresses of
    // its tag members stay valid for the lifetime of the epoll set even though
    // the owning unique_ptr moves into loops_ below.
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = &loop->listen_tag;
    ::epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->listen_fd, &ev);

    ev.data.ptr = &loop->wake_tag;
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

  // Make `wanted` this connection's epoll interest set. Does nothing when that
  // is already the registered set -- the case that matters, because flush()
  // ends every keep-alive request wanting exactly the EPOLLIN it already has.
  // ADD when the fd is outside the set (fresh accept, or handed back by a
  // worker), MOD otherwise. Returns false only if epoll_ctl failed.
  auto arm = [&](Connection* conn, std::uint32_t wanted) -> bool {
    if (conn->armed == wanted) return true;
    epoll_event ev{};
    ev.events = wanted;
    ev.data.ptr = conn;
    const int op = conn->armed == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (::epoll_ctl(loop.epoll_fd, op, conn->fd, &ev) != 0) return false;
    conn->armed = wanted;
    return true;
  };

  auto disarm = [&](Connection* conn) {
    if (conn->armed == 0) return;
    ::epoll_ctl(loop.epoll_fd, EPOLL_CTL_DEL, conn->fd, nullptr);
    conn->armed = 0;
  };

  // Connections torn down during the current batch of events. Reclaiming a
  // Connection the moment it dies is not safe now that events carry raw
  // Connection pointers: epoll_wait already snapshotted this batch, so a later
  // event in it can name a connection that an earlier one killed (a completion
  // drained on wake_fd closes arbitrary connections, and any of them may have
  // their own EPOLLIN sitting further down the same array). Holding the fd
  // open until the batch drains additionally stops accept4() from handing the
  // same descriptor number to a new connection, which would alias a stale
  // event -- a stale EPOLLHUP included -- onto a live one.
  std::vector<int> pending_close;

  auto close_connection = [&](Connection* conn) {
    if (conn->closed) return;
    conn->closed = true;
    disarm(conn);
    pending_close.push_back(conn->fd);
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
        arm(conn, static_cast<std::uint32_t>(EPOLLIN | EPOLLOUT));
        return true;  // finish later
      }
      if (n < 0 && errno == EINTR) continue;
      return false;
    }

    conn->out.clear();
    conn->out_sent = 0;
    if (conn->close_after) return false;

    // Usually a no-op: EPOLLOUT is only ever armed by the partial-write branch
    // above, so the interest set is already EPOLLIN on the common path.
    arm(conn, static_cast<std::uint32_t>(EPOLLIN));
    return true;
  };

  // Answer a protocol error and close. Framing is unknown past this point --
  // a wrong Content-Length means we cannot know where the next request would
  // start -- so the connection is never kept alive after a reject.
  auto reject = [&](Connection* conn, int status, std::string_view body) -> bool {
    conn->close_after = true;
    build_response(conn->out, status, body, /*keep_alive=*/false);
    conn->out_sent = 0;
    return flush(conn);  // false once fully sent -> caller closes
  };

  // Parse and dispatch as many complete requests as the buffer holds.
  auto process = [&](Connection* conn) -> bool {
    for (;;) {
      if (!conn->out.empty()) return true;  // still draining a response

      const std::size_t head_end = conn->in.find("\r\n\r\n");
      if (head_end == std::string::npos) {
        if (conn->in.size() > kMaxHeaderBytes) {
          return reject(conn, 431, "{\"error\":\"headers too large\"}");
        }
        return true;  // need more bytes
      }
      if (head_end > kMaxHeaderBytes) {
        return reject(conn, 431, "{\"error\":\"headers too large\"}");
      }

      const std::string_view head(conn->in.data(), head_end);

      // Request line: METHOD SP TARGET SP VERSION
      std::size_t line_end = head.find("\r\n");
      if (line_end == std::string_view::npos) line_end = head.size();
      const std::string_view request_line = head.substr(0, line_end);
      const std::size_t sp1 = request_line.find(' ');
      const std::size_t sp2 =
          sp1 == std::string_view::npos ? std::string_view::npos
                                        : request_line.find(' ', sp1 + 1);
      if (sp2 == std::string_view::npos || sp2 + 1 >= request_line.size() ||
          request_line.substr(sp2 + 1, 5) != "HTTP/") {
        return reject(conn, 400, "{\"error\":\"malformed request line\"}");
      }

      Request req;
      req.method = request_line.substr(0, sp1);
      std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);

      const std::size_t qmark = target.find('?');
      if (qmark == std::string_view::npos) {
        req.path = target;
        req.query = {};
      } else {
        req.path = target.substr(0, qmark);
        req.query = target.substr(qmark + 1);
      }

      // HTTP/1.1 is keep-alive unless the client says otherwise.
      bool keep_alive = request_line.substr(sp2 + 1) != "HTTP/1.0";

      // Headers, one line at a time -- never a substring hunt over the whole
      // block, which would match "X-Content-Length:", miss mixed case, and
      // silently take the first of two conflicting values. Content-Length is
      // the request's framing: parsed strictly (digits only, no sign, bounded,
      // duplicates must agree) or the request dies with 400/413 rather than
      // this thread guessing where the next request starts.
      bool cl_seen = false;
      std::size_t content_length = 0;
      int error_status = 0;
      const char* error_body = nullptr;
      std::size_t cursor = line_end;
      while (cursor < head.size()) {
        cursor += 2;  // the "\r\n" that ended the previous line
        std::size_t next = head.find("\r\n", cursor);
        if (next == std::string_view::npos) next = head.size();
        const std::string_view line = head.substr(cursor, next - cursor);
        cursor = next;

        const std::size_t colon = line.find(':');
        if (colon == 0 || colon == std::string_view::npos) {
          error_status = 400;
          error_body = "{\"error\":\"malformed header\"}";
          break;
        }
        const std::string_view name = line.substr(0, colon);
        // Whitespace inside a header name ("Content-Length : 5") is a classic
        // smuggling shape: one parser sees the header, another does not.
        if (name.find(' ') != std::string_view::npos ||
            name.find('\t') != std::string_view::npos) {
          error_status = 400;
          error_body = "{\"error\":\"malformed header\"}";
          break;
        }
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
          value.remove_prefix(1);
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
          value.remove_suffix(1);

        if (iequals(name, "content-length")) {
          if (value.empty()) {
            error_status = 400;
            error_body = "{\"error\":\"malformed content-length\"}";
            break;
          }
          std::size_t parsed = 0;
          bool digits_ok = true;
          bool too_large = false;
          for (const char c : value) {
            if (c < '0' || c > '9') {
              digits_ok = false;
              break;
            }
            if (!too_large) {
              // Saturating accumulate: once past the cap the exact value no
              // longer matters, and the loop can never overflow size_t.
              parsed = parsed * 10 + static_cast<std::size_t>(c - '0');
              if (parsed > kMaxBodyBytes) too_large = true;
            }
          }
          if (!digits_ok) {
            error_status = 400;
            error_body = "{\"error\":\"malformed content-length\"}";
            break;
          }
          if (too_large) {
            error_status = 413;
            error_body = "{\"error\":\"payload too large\"}";
            break;
          }
          if (cl_seen && parsed != content_length) {
            error_status = 400;
            error_body = "{\"error\":\"conflicting content-length\"}";
            break;
          }
          cl_seen = true;
          content_length = parsed;
        } else if (iequals(name, "transfer-encoding")) {
          // Not implemented. Accepting it while reading Content-Length bytes
          // is exactly how request smuggling works, so it is a hard reject.
          error_status = 400;
          error_body = "{\"error\":\"transfer-encoding not supported\"}";
          break;
        } else if (iequals(name, "connection")) {
          if (icontains(value, "close")) keep_alive = false;
        }
      }
      if (error_status != 0) return reject(conn, error_status, error_body);

      const std::size_t body_start = head_end + 4;
      // Subtraction, never body_start + content_length: the sum was the
      // overflow that let a 20-digit Content-Length wrap size_t.
      if (conn->in.size() - body_start < content_length) return true;  // need more
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
        disarm(conn);
        return true;
      }

      conn->close_after = !keep_alive;
      build_response(conn->out, status, body, keep_alive);
      conn->out_sent = 0;
      // Only flush() may decide a connection is finished. Returning false here
      // whenever close_after was set would tear down a response that had only
      // partially drained -- flush() signals "done, close now" by returning
      // false itself, and "still draining, EPOLLOUT armed" by returning true.
      if (!flush(conn)) return false;
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

      // Test hook, unset in production. The kernel autotunes the send buffer
      // to megabytes, so every response this service produces fits in one
      // send() and the partial-write path is unreachable over loopback.
      // Pinning the buffer small makes backpressure reproducible -- see the
      // partial-write case in tests/http_test.cpp. Setting SO_SNDBUF also
      // disables autotuning, which is the point.
      if (forced_sndbuf > 0) {
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &forced_sndbuf,
                     sizeof(forced_sndbuf));
      }

      auto conn = std::make_unique<Connection>();
      conn->fd = fd;
      conn->in.reserve(kReadChunk);

      // arm() before the map takes ownership, so a failed ADD costs only the
      // fd -- the Connection is dropped here rather than needing an erase.
      Connection* raw = conn.get();
      if (!arm(raw, static_cast<std::uint32_t>(EPOLLIN))) {
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
      // Torn down earlier in this batch and awaiting reclamation.
      if (conn->closed) continue;

      conn->deferred = false;
      conn->close_after = !done.keep_alive;
      build_response(conn->out, done.status, done.body, done.keep_alive);
      conn->out_sent = 0;

      // Back into the epoll set before writing, so a partial write can arm
      // EPOLLOUT. process() disarmed it when it deferred, so this is an ADD.
      arm(conn, static_cast<std::uint32_t>(EPOLLIN));

      // flush() returns false once a close_after response is fully drained;
      // checking close_after here as well would tear the connection down in
      // the middle of a partial write and truncate the response.
      if (!flush(conn)) {
        close_connection(conn);
        continue;
      }
      if (!process(conn)) close_connection(conn);
    }
  };

  while (running_.load(std::memory_order_relaxed)) {
    const int n = ::epoll_wait(loop.epoll_fd, events, kMaxEvents, 200);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    for (int i = 0; i < n; ++i) {
      void* const tag = events[i].data.ptr;

      if (tag == &loop.listen_tag) {
        accept_new();
        continue;
      }

      if (tag == &loop.wake_tag) {
        std::uint64_t counter = 0;
        while (::read(loop.wake_fd, &counter, sizeof(counter)) > 0) {
        }
        drain_completions();
        continue;
      }

      // Everything else is a connection, named directly rather than through a
      // hash of its fd. Safe against a stale pointer because close_connection()
      // defers reclamation past the end of this batch.
      Connection* conn = static_cast<Connection*>(tag);
      if (conn->closed) continue;

      if (events[i].events & (EPOLLHUP | EPOLLERR)) {
        if (!conn->deferred) close_connection(conn);
        continue;
      }

      if (events[i].events & EPOLLOUT) {
        // Same rule as drain_completions: only a finished (or dead) flush may
        // close -- flush() itself signals that by returning false.
        if (!flush(conn)) {
          close_connection(conn);
          continue;
        }
        // Draining may have unblocked requests that were already read into
        // conn->in behind the response we were writing. epoll is level
        // triggered on the *socket*, and those bytes have long since left it,
        // so no future EPOLLIN will arrive to prompt this -- without parsing
        // here, pipelined requests behind a backpressured write are never
        // answered at all.
        if (conn->out.empty() && !conn->deferred && !process(conn)) {
          close_connection(conn);
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
          close_connection(conn);
          continue;
        }
      }
    }

    // End of batch: no pointer from `events` is live any more, so the closed
    // connections can be freed and their descriptors released for reuse.
    for (const int fd : pending_close) {
      ::close(fd);
      loop.connections.erase(fd);
    }
    pending_close.clear();
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
