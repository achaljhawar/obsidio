// Socket-level tests for the HTTP front end.
//
// These speak raw bytes to a real obsidio-server process, because the things
// under test are precisely the ones a well-behaved HTTP client cannot express:
// a 20-digit Content-Length, two conflicting Content-Length headers, a header
// name with a space in it, a body that never arrives. curl will not send those;
// an attacker will.
//
// Every case asserts a specific status code. The rule the whole file is built
// around: a malformed request must be answered and closed, never crash the
// process and never be silently mis-framed into the next request on the same
// connection.
//
// Usage: obsidio-http-test <path-to-obsidio-server>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr std::uint16_t kPort = 18099;
int g_failures = 0;
int g_checks = 0;

int connect_server() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  // Bounded: a request the server is (correctly) still waiting on must not
  // hang the test run.
  timeval tv{};
  tv.tv_sec = 2;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

// Send raw bytes, read whatever comes back until EOF or the read timeout.
std::string exchange(const std::string& request) {
  const int fd = connect_server();
  if (fd < 0) return "<connect-failed>";
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent,
                             MSG_NOSIGNAL);
    if (n <= 0) break;
    sent += static_cast<std::size_t>(n);
  }
  std::string out;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    out.append(buf, static_cast<std::size_t>(n));
    // A complete response whose body we already hold: stop rather than wait
    // out the timeout on a kept-alive connection.
    const std::size_t head_end = out.find("\r\n\r\n");
    if (head_end != std::string::npos) {
      const std::size_t cl = out.find("Content-Length: ");
      if (cl != std::string::npos && cl < head_end) {
        const std::size_t len = std::strtoul(out.c_str() + cl + 16, nullptr, 10);
        if (out.size() >= head_end + 4 + len) break;
      }
    }
  }
  ::close(fd);
  return out;
}

// First status code in a response, or 0 when nothing came back.
int status_of(const std::string& response) {
  if (response.rfind("HTTP/1.1 ", 0) != 0) return 0;
  return std::atoi(response.c_str() + 9);
}

void check_status(const char* label, const std::string& request, int want) {
  ++g_checks;
  const std::string response = exchange(request);
  const int got = status_of(response);
  if (got == want) {
    std::printf("  ok    %s -> %d\n", label, got);
    return;
  }
  std::printf("  FAIL  %s\n        want status %d, got %d\n", label, want, got);
  if (response.empty()) {
    std::printf("        (no response; connection closed silently)\n");
  } else {
    const std::size_t line_end = response.find("\r\n");
    std::printf("        first line: %.*s\n",
                static_cast<int>(line_end == std::string::npos ? response.size()
                                                              : line_end),
                response.c_str());
  }
  ++g_failures;
}

void check_true(const char* label, bool cond) {
  ++g_checks;
  if (cond) {
    std::printf("  ok    %s\n", label);
    return;
  }
  std::printf("  FAIL  %s\n", label);
  ++g_failures;
}

std::string get(const char* target) {
  return std::string("GET ") + target + " HTTP/1.1\r\nHost: t\r\n\r\n";
}

std::string post_price(const std::string& body) {
  char head[256];
  std::snprintf(head, sizeof(head),
                "POST /price HTTP/1.1\r\nHost: t\r\nContent-Type: "
                "application/json\r\nContent-Length: %zu\r\n\r\n",
                body.size());
  return std::string(head) + body;
}

bool wait_ready() {
  for (int i = 0; i < 100; ++i) {
    const int fd = connect_server();
    if (fd >= 0) {
      ::close(fd);
      const std::string r = exchange(get("/health"));
      if (status_of(r) == 200) return true;
    }
    usleep(100 * 1000);
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: %s <path-to-obsidio-server>\n", argv[0]);
    return 2;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::perror("fork");
    return 2;
  }
  if (pid == 0) {
    char port[16];
    std::snprintf(port, sizeof(port), "%u", kPort);
    ::setenv("PORT", port, 1);
    ::setenv("IO_THREADS", "1", 1);
    ::setenv("RISK_WORKERS", "1", 1);
    // Pin the send buffer small so a large response cannot drain in one
    // send(). Without this the kernel autotunes to megabytes and the
    // partial-write path below is simply unreachable over loopback. Every
    // other response in this file is ~100 bytes and unaffected.
    ::setenv("OBSIDIO_SNDBUF", "4096", 1);
    // The chain back end is irrelevant here and the slow one costs seconds.
    ::setenv("RISK_BACKEND", "reference", 1);
    ::execl(argv[1], argv[1], static_cast<char*>(nullptr));
    std::perror("execl");
    _exit(127);
  }

  if (!wait_ready()) {
    std::printf("FAIL: server never became ready on port %u\n", kPort);
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return 1;
  }

  std::printf("Baseline: the happy paths still work\n");
  check_status("GET /health", get("/health"), 200);
  check_status("GET /price?symbol=AAPL", get("/price?symbol=AAPL"), 200);
  check_status("GET /stats?symbol=AAPL", get("/stats?symbol=AAPL"), 200);
  check_status("GET /nope", get("/nope"), 404);
  check_status("POST /price valid", post_price("{\"symbol\":\"AAPL\",\"price\":190.5}"),
               200);
  check_status("POST /price valid negative",
               post_price("{\"symbol\":\"AAPL\",\"price\":-3}"), 200);
  check_status("POST /price valid exponent",
               post_price("{\"symbol\":\"AAPL\",\"price\":1.5e2}"), 200);

  std::printf("\nContent-Length parsed strictly, without overflow\n");
  // 20 digits: overflows a 64-bit size_t. The old parse wrapped this to a
  // small number and mis-framed the body.
  check_status(
      "CL 99999999999999999999999 (overflows size_t)",
      "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: "
      "99999999999999999999999\r\n\r\n{}",
      413);
  check_status("CL 18446744073709551616 (2^64)",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: "
               "18446744073709551616\r\n\r\n{}",
               413);
  check_status("CL negative",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: -1\r\n\r\n{}",
               400);
  check_status("CL non-numeric",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: abc\r\n\r\n{}",
               400);
  check_status("CL trailing garbage",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: 5x\r\n\r\n{}",
               400);
  check_status("CL plus-signed",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: +5\r\n\r\n{}",
               400);
  check_status("CL empty",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: \r\n\r\n{}",
               400);
  check_status("CL whitespace-padded value is accepted",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length:   27   "
               "\r\n\r\n{\"symbol\":\"AAPL\",\"price\":1}",
               200);

  std::printf("\nConflicting and decoy Content-Length headers\n");
  check_status("two conflicting CL headers",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: "
               "5\r\nContent-Length: 6\r\n\r\n{}",
               400);
  check_status("two agreeing CL headers",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: "
               "27\r\nContent-Length: 27\r\n\r\n{\"symbol\":\"AAPL\",\"price\":1}",
               200);
  check_status("mixed-case duplicate disagrees",
               "POST /price HTTP/1.1\r\nHost: t\r\ncontent-length: "
               "5\r\nCONTENT-LENGTH: 7\r\n\r\n{}",
               400);
  // X-Content-Length must not be mistaken for framing: the old substring scan
  // matched it, took "999", and then waited forever for a body.
  check_status("X-Content-Length decoy is ignored",
               "GET /health HTTP/1.1\r\nHost: t\r\nX-Content-Length: 999\r\n\r\n",
               200);
  check_status("header name with space before colon",
               "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length : 5\r\n\r\n{}",
               400);
  check_status("Transfer-Encoding is refused, not ignored",
               "POST /price HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: "
               "chunked\r\n\r\n0\r\n\r\n",
               400);

  std::printf("\nSize limits\n");
  {
    // Declared body far over the cap: rejected on the header alone, without
    // buffering a byte of it.
    check_status("body over cap (declared 1 MiB)",
                 "POST /price HTTP/1.1\r\nHost: t\r\nContent-Length: "
                 "1048576\r\n\r\n",
                 413);
    std::string big = "GET /health HTTP/1.1\r\nHost: t\r\n";
    while (big.size() < 20 * 1024) big += "X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n";
    big += "\r\n";
    check_status("headers over 16 KiB", big, 431);
  }

  std::printf("\nMalformed request lines and headers\n");
  check_status("no HTTP version", "GET /health\r\n\r\n", 400);
  check_status("garbage request line", "not-a-request\r\n\r\n", 400);
  check_status("empty request line", "\r\n\r\n", 400);
  check_status("header without colon",
               "GET /health HTTP/1.1\r\nHost: t\r\nBrokenHeader\r\n\r\n", 400);
  check_status("header with empty name",
               "GET /health HTTP/1.1\r\nHost: t\r\n: value\r\n\r\n", 400);

  std::printf("\nPrice values: NaN, Infinity, and malformed numbers\n");
  check_status("NaN", post_price("{\"symbol\":\"AAPL\",\"price\":NaN}"), 400);
  check_status("nan lowercase", post_price("{\"symbol\":\"AAPL\",\"price\":nan}"),
               400);
  check_status("Infinity", post_price("{\"symbol\":\"AAPL\",\"price\":Infinity}"),
               400);
  check_status("-Infinity", post_price("{\"symbol\":\"AAPL\",\"price\":-Infinity}"),
               400);
  check_status("inf", post_price("{\"symbol\":\"AAPL\",\"price\":inf}"), 400);
  check_status("1e999 overflows to inf",
               post_price("{\"symbol\":\"AAPL\",\"price\":1e999}"), 400);
  check_status("hex literal", post_price("{\"symbol\":\"AAPL\",\"price\":0x10}"),
               400);
  check_status("two decimal points",
               post_price("{\"symbol\":\"AAPL\",\"price\":1.2.3}"), 400);
  check_status("trailing letters",
               post_price("{\"symbol\":\"AAPL\",\"price\":5abc}"), 400);
  check_status("bare minus", post_price("{\"symbol\":\"AAPL\",\"price\":-}"), 400);
  check_status("empty exponent",
               post_price("{\"symbol\":\"AAPL\",\"price\":1e}"), 400);
  check_status("leading dot", post_price("{\"symbol\":\"AAPL\",\"price\":.5}"), 400);
  check_status("quoted number is not a number",
               post_price("{\"symbol\":\"AAPL\",\"price\":\"190\"}"), 400);
  check_status("missing price field", post_price("{\"symbol\":\"AAPL\"}"), 400);
  check_status("unknown symbol",
               post_price("{\"symbol\":\"ZZZZ\",\"price\":1}"), 404);

  std::printf("\nA rejected request never poisons stored state\n");
  {
    // The NaN attempts above must not have reached the price table: if one had,
    // this response would contain bare `nan`, which is not valid JSON.
    const std::string r = exchange(get("/price?symbol=AAPL"));
    check_true("GET /price returns no nan/inf after rejected writes",
               r.find("nan") == std::string::npos &&
                   r.find("inf") == std::string::npos &&
                   status_of(r) == 200);
    const std::string s = exchange(get("/stats?symbol=AAPL"));
    check_true("GET /stats returns no nan/inf after rejected writes",
               s.find("nan") == std::string::npos &&
                   s.find("inf") == std::string::npos &&
                   status_of(s) == 200);
  }

  std::printf("\nBackpressure: no response is dropped or truncated\n");
  {
    // Two bugs lived here, both from treating a partially-drained write as a
    // finished one:
    //
    //   * A `Connection: close` response whose first send() returned EAGAIN
    //     was abandoned mid-flight -- observed writing 0 of 211 bytes -- so
    //     the client got a clean FIN and never saw its answer at all.
    //   * After a backpressured write drained, requests already sitting in
    //     the read buffer behind it were never parsed, because epoll is level
    //     triggered on the socket and those bytes had long since left it.
    //
    // Both need a full send buffer to reproduce, which is why the server runs
    // with OBSIDIO_SNDBUF pinned above: the kernel otherwise autotunes to
    // megabytes and no response this service produces can ever fill it.
    //
    // The invariant is the one that matters to a client: every pipelined
    // request gets a complete, correctly-framed response, however the writes
    // happened to break up. Depths are chosen to straddle the 4 KiB buffer at
    // ~211 bytes per response -- N=8 fits comfortably, the rest do not.
    for (const int depth : {8, 24, 32, 40, 48, 64, 96}) {
      std::string batch;
      for (int i = 0; i < depth - 1; ++i) {
        batch += "GET /stats?symbol=AAPL HTTP/1.1\r\nHost: t\r\n\r\n";
      }
      batch +=
          "GET /stats?symbol=AAPL HTTP/1.1\r\nHost: t\r\nConnection: "
          "close\r\n\r\n";

      const int fd = connect_server();
      if (fd < 0) {
        check_true("connect for backpressure test", false);
        continue;
      }
      std::size_t sent = 0;
      while (sent < batch.size()) {
        const ssize_t n =
            ::send(fd, batch.data() + sent, batch.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += static_cast<std::size_t>(n);
      }
      // Deliberately do not read while the server writes: this is what fills
      // the send buffer and forces the partial-write path.
      usleep(60 * 1000);

      std::string acc;
      char buf[8192];
      for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, static_cast<std::size_t>(n));
      }
      ::close(fd);

      // Walk the stream response by response; a short final body or a
      // dangling partial header both count as corruption.
      int complete = 0;
      bool corrupt = false;
      std::size_t pos = 0;
      for (;;) {
        const std::size_t he = acc.find("\r\n\r\n", pos);
        if (he == std::string::npos) {
          corrupt = pos < acc.size();  // trailing bytes that are not a response
          break;
        }
        std::size_t declared = 0;
        const std::size_t cl = acc.find("Content-Length: ", pos);
        if (cl != std::string::npos && cl < he) {
          declared = std::strtoul(acc.c_str() + cl + 16, nullptr, 10);
        }
        if (he + 4 + declared > acc.size()) {
          corrupt = true;  // body shorter than its own Content-Length
          break;
        }
        pos = he + 4 + declared;
        ++complete;
      }

      char label[96];
      std::snprintf(label, sizeof(label),
                    "pipeline depth %d: all %d responses complete", depth,
                    depth);
      if (complete != depth || corrupt) {
        std::printf("        depth %d: %d complete, corrupt=%d, %zu bytes\n",
                    depth, complete, corrupt ? 1 : 0, acc.size());
      }
      check_true(label, complete == depth && !corrupt);
    }
  }

  std::printf("\nFraming survives: keep-alive and pipelining still work\n");
  {
    const int fd = connect_server();
    check_true("connect for keep-alive test", fd >= 0);
    if (fd >= 0) {
      // Two requests in one write: the second is only answered if the first
      // consumed exactly its own bytes and no more.
      const std::string two = get("/health") + get("/price?symbol=AAPL");
      ::send(fd, two.data(), two.size(), MSG_NOSIGNAL);
      std::string acc;
      char buf[4096];
      for (int i = 0; i < 8; ++i) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acc.append(buf, static_cast<std::size_t>(n));
        std::size_t count = 0;
        for (std::size_t p = acc.find("HTTP/1.1 "); p != std::string::npos;
             p = acc.find("HTTP/1.1 ", p + 1)) {
          ++count;
        }
        if (count >= 2) break;
      }
      std::size_t responses = 0;
      for (std::size_t p = acc.find("HTTP/1.1 "); p != std::string::npos;
           p = acc.find("HTTP/1.1 ", p + 1)) {
        ++responses;
      }
      check_true("pipelined GETs both answered on one connection",
                 responses == 2);
      ::close(fd);
    }
  }

  std::printf("\nThe server is still alive after every one of those\n");
  check_status("GET /health after the whole suite", get("/health"), 200);
  {
    int status = 0;
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    check_true("server process did not crash or exit", r == 0);
  }

  ::kill(pid, SIGTERM);
  int wstatus = 0;
  ::waitpid(pid, &wstatus, 0);

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) {
    std::printf("all checks passed\n");
    return 0;
  }
  return 1;
}
