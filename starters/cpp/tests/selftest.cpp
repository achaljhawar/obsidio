// Correctness gate for the hash path.
//
// Run this after EVERY change to sha256.cpp or risk.cpp. A /risk response with
// a wrong digest scores zero no matter how fast it was, and the failure is
// silent -- the response still looks like a hex string. This test is the only
// thing standing between an optimisation and a zero.
//
// Golden values generated with Python's hashlib; cross-check any of them with:
//   python3 -c "import hashlib;h='0.5';[h:=hashlib.sha256(h.encode()).hexdigest() for _ in range(50000)];print(h)"
#include <cstdio>
#include <string>

#include "../src/risk.hpp"
#include "../src/sha256.hpp"

namespace {

int g_failures = 0;

void expect_eq(const std::string& label, const std::string& got,
               const std::string& want) {
  if (got == want) {
    std::printf("  ok    %s\n", label.c_str());
    return;
  }
  std::printf("  FAIL  %s\n        got  %s\n        want %s\n", label.c_str(),
              got.c_str(), want.c_str());
  ++g_failures;
}

std::string sha256_hex(const std::string& input) {
  std::uint8_t digest[obsidio::kSha256DigestBytes];
  obsidio::sha256(reinterpret_cast<const std::uint8_t*>(input.data()),
                  input.size(), digest);
  char hex[obsidio::kSha256HexBytes];
  obsidio::hex_encode(digest, obsidio::kSha256DigestBytes, hex);
  return std::string(hex, obsidio::kSha256HexBytes);
}

}  // namespace

int main() {
  std::printf("SHA-256 primitive (FIPS 180-4 vectors)\n");
  expect_eq("empty string", sha256_hex(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  expect_eq("\"abc\"", sha256_hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  expect_eq("448-bit message",
            sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // A 64-byte input is the exact case the /risk loop hits on every iteration
  // after the first: two blocks, second block all padding.
  expect_eq("64-byte input (two-block boundary)",
            sha256_hex(std::string(64, 'a')),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

  std::printf("\nRisk chain, short runs\n");
  expect_eq("seed=0.5    n=1", obsidio::risk_hash("0.5", 1),
            "d2cbad71ff333de67d07ec676e352ab7f38248eb69c942950157220607c55e84");
  expect_eq("seed=0.5    n=2", obsidio::risk_hash("0.5", 2),
            "ac6f81b4b1b1f30ca677965326ca21ea78e58a9b227ba72f05dc74c12eb64049");
  expect_eq("seed=0.5    n=10", obsidio::risk_hash("0.5", 10),
            "0d7d5afa9207bab8203501c8a7505d8e13b0ada58ec966c3f9b1c794e59308c5");
  expect_eq("seed=none   n=10", obsidio::risk_hash("none", 10),
            "c15c75ca43ca0c34a553598c11847525bf458005611493c6c795e5dbd401ee57");
  expect_eq("seed=0.4821 n=10", obsidio::risk_hash("0.4821", 10),
            "39cccfc2d94087e4df1dccf10ee55d9b7221d48919e065cad1ee933b5fd546c4");

  std::printf("\nRisk chain, full %d iterations (the graded path)\n",
              obsidio::kRiskIterations);
  expect_eq("seed=0.5", obsidio::risk_hash("0.5"),
            "8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148");
  expect_eq("seed=none", obsidio::risk_hash("none"),
            "5b83a4893dc72cfd45dabf4fae920510e38954cafa2481efce3f7815d09bc460");

  // -------------------------------------------------------------------------
  // The paired path the risk pool actually runs at peak. Two chains sharing one
  // thread is exactly where a lane-crossing bug lives, and such a bug produces
  // two perfectly plausible 64-char hex strings, so it has to be checked
  // against the same goldens as the single path.
  std::printf("\nRisk chain, x2 interleaved (what the pool runs at peak)\n");
  {
    std::string a, b;

    obsidio::risk_hash_x2("0.5", "none", a, b);
    expect_eq("x2 lane A  seed=0.5", a,
              "8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148");
    expect_eq("x2 lane B  seed=none", b,
              "5b83a4893dc72cfd45dabf4fae920510e38954cafa2481efce3f7815d09bc460");

    // Swapped, to catch a back end that silently returns lane A twice.
    obsidio::risk_hash_x2("none", "0.5", a, b);
    expect_eq("x2 swapped A  seed=none", a,
              "5b83a4893dc72cfd45dabf4fae920510e38954cafa2481efce3f7815d09bc460");
    expect_eq("x2 swapped B  seed=0.5", b,
              "8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148");

    // Identical seeds must give identical digests, short chains must match the
    // single-chain path, and n=1 exercises the zero-rounds edge.
    obsidio::risk_hash_x2("0.5", "0.5", a, b);
    expect_eq("x2 same seed both lanes", a, b);
    obsidio::risk_hash_x2("0.5", "0.4821", a, b, 10);
    expect_eq("x2 n=10 lane A", a, obsidio::risk_hash("0.5", 10));
    expect_eq("x2 n=10 lane B", b, obsidio::risk_hash("0.4821", 10));
    obsidio::risk_hash_x2("0.5", "none", a, b, 1);
    expect_eq("x2 n=1 lane A", a, obsidio::risk_hash("0.5", 1));
    expect_eq("x2 n=1 lane B", b, obsidio::risk_hash("none", 1));
  }

  std::printf("\nRisk chain, x3 interleaved (what the pool runs at peak)\n");
  {
    const std::string kSeed05 = obsidio::risk_hash("0.5");
    const std::string kSeedNone = obsidio::risk_hash("none");
    std::string a, b, c;

    obsidio::risk_hash_x3("0.5", "none", "0.4821", a, b, c);
    expect_eq("x3 lane A  seed=0.5", a, kSeed05);
    expect_eq("x3 lane B  seed=none", b, kSeedNone);
    expect_eq("x3 lane C  seed=0.4821", c, obsidio::risk_hash("0.4821"));

    // Rotate the seeds: a lane that quietly reads its neighbour's state passes
    // one ordering and fails another.
    obsidio::risk_hash_x3("0.4821", "0.5", "none", a, b, c);
    expect_eq("x3 rotated A  seed=0.4821", a, obsidio::risk_hash("0.4821"));
    expect_eq("x3 rotated B  seed=0.5", b, kSeed05);
    expect_eq("x3 rotated C  seed=none", c, kSeedNone);

    obsidio::risk_hash_x3("0.5", "0.5", "0.5", a, b, c);
    expect_eq("x3 same seed all lanes A==B", a, b);
    expect_eq("x3 same seed all lanes B==C", b, c);

    // x3 must agree with x2 and x1 on the lanes they share, or the pool serves
    // different digests depending on how deep the queue happened to be.
    std::string xa, xb;
    obsidio::risk_hash_x2("0.5", "none", xa, xb);
    obsidio::risk_hash_x3("0.5", "none", "0.4821", a, b, c);
    expect_eq("x3 agrees with x2 lane A", a, xa);
    expect_eq("x3 agrees with x2 lane B", b, xb);

    obsidio::risk_hash_x3("0.5", "0.4821", "none", a, b, c, 10);
    expect_eq("x3 n=10 lane A", a, obsidio::risk_hash("0.5", 10));
    expect_eq("x3 n=10 lane B", b, obsidio::risk_hash("0.4821", 10));
    expect_eq("x3 n=10 lane C", c, obsidio::risk_hash("none", 10));
    obsidio::risk_hash_x3("0.5", "none", "0.4821", a, b, c, 1);
    expect_eq("x3 n=1 lane A", a, obsidio::risk_hash("0.5", 1));
    expect_eq("x3 n=1 lane B", b, obsidio::risk_hash("none", 1));
    expect_eq("x3 n=1 lane C", c, obsidio::risk_hash("0.4821", 1));
  }

  // -------------------------------------------------------------------------
  // A rejected back end is not a correctness failure -- risk.cpp falls back to
  // the reference chain and every digest above still passes -- but it silently
  // costs ~7x throughput, which is the whole competition. On an architecture
  // that HAS an accelerated back end, rejection means the back end is buggy,
  // and that must fail the build rather than ship quietly slow.
  std::printf("\nHash back end\n");
  std::printf("  selected: %s\n", obsidio::risk_backend_name());
  if (obsidio::risk_backend_rejected()) {
    std::printf(
        "  FAIL  an accelerated back end was available but FAILED "
        "self-verification\n");
    ++g_failures;
  }

  if (g_failures == 0) {
    std::printf("\nall checks passed\n");
    return 0;
  }
  std::printf("\n%d check(s) FAILED\n", g_failures);
  return 1;
}
