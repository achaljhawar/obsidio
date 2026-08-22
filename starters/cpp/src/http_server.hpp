// Small epoll HTTP/1.1 server, dependency-free. N IO threads, each with its
// own epoll instance and SO_REUSEPORT listener; a connection is owned
// start-to-finish by the thread that accepted it, so the hot path has no
// locks. Slow work defers to a pool and answers later via complete().
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace obsidio {

struct Request {
  std::string_view method;
  std::string_view path;   // path without the query string
  std::string_view query;  // raw query string, no leading '?'
  std::string_view body;
};

// Identifies a connection well enough to answer it later from another thread.
struct DeferContext {
  int loop_index{};
  int fd{-1};
  bool keep_alive{true};
};

// Return true if `out` holds the response body to send now; false to take
// ownership and eventually call Server::complete() with the same context.
using Handler =
    std::function<bool(const Request&, const DeferContext&, std::string& out,
                       int& status)>;

class Server {
 public:
  Server(std::uint16_t port, std::size_t io_threads, Handler handler);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds listening sockets. Returns false on failure (message on stderr).
  bool start();

  // Blocks until stop() is called.
  void run();

  // Signals the IO loops to exit. Async-signal-safe.
  void stop();

  // Finish a deferred request. Safe from any thread.
  void complete(const DeferContext& ctx, int status, std::string body);

  // Build a full HTTP response (status line + headers + body) into `out`.
  static void build_response(std::string& out, int status,
                             std::string_view body, bool keep_alive);

 private:
  struct Loop;
  struct Connection;

  void loop_main(std::size_t index);
  bool bind_listener(Loop& loop);

  std::uint16_t port_;
  std::size_t io_threads_;
  Handler handler_;
  std::vector<std::unique_ptr<Loop>> loops_;
  std::atomic<bool> running_{false};
};

}  // namespace obsidio
