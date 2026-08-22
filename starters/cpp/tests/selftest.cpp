// Correctness gate for the hash path.
//
// Run this after EVERY change to sha256.cpp or risk.cpp. A /risk response with
// a wrong digest scores zero no matter how fast it was, and the failure is
// silent -- the response still looks like a hex string. This test is the only
// thing standing between an optimisation and a zero.
//
// Golden values generated with Python's hashlib; cross-check any of them with:
//   python3 -c "import hashlib;h='0.5';[h:=hashlib.sha256(h.encode()).hexdigest() for _ in range(50000)];print(h)"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/risk.hpp"
#include "../src/sha256.hpp"

namespace {

int g_failures{};

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

std::string sha256_64_hex(const std::string& input) {
  std::uint8_t digest[obsidio::kSha256DigestBytes];
  obsidio::sha256_64(reinterpret_cast<const std::uint8_t*>(input.data()), digest);
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

  // sha256_64 is the specialised two-block path the reference chain runs on
  // every round after the first. It must be byte-identical to the generic one
  // for every 64-byte input, or the digest silently diverges and every /risk
  // request served from the fallback scores zero.
  //
  // This is what lets chain_fallback() use it while chain_reference() -- the
  // oracle that validates the accelerated back ends -- stays on the generic
  // primitive. The two chains differ by exactly this substitution, so pinning
  // the primitive pins the chain.
  std::printf("\nSpecialised 64-byte path (sha256_64 == sha256)\n");
  expect_eq("64 x 'a'", sha256_64_hex(std::string(64, 'a')),
            sha256_hex(std::string(64, 'a')));
  expect_eq("64 x 0x00", sha256_64_hex(std::string(64, '\0')),
            sha256_hex(std::string(64, '\0')));
  expect_eq("64 x 0xff", sha256_64_hex(std::string(64, '\xff')),
            sha256_hex(std::string(64, '\xff')));
  {
    // A byte sweep across the block, and a real chain value.
    std::string mixed(64, '\0');
    for (int i{}; i < 64; ++i) mixed[i] = static_cast<char>(i * 4 + 1);
    expect_eq("byte sweep", sha256_64_hex(mixed), sha256_hex(mixed));
    const std::string chained{sha256_hex("0.5")};
    expect_eq("live chain value", sha256_64_hex(chained), sha256_hex(chained));
    // Every hex character, since that is all the chain ever feeds it.
    std::string hexish(64, '0');
    for (int i{}; i < 64; ++i) hexish[i] = "0123456789abcdef"[i % 16];
    expect_eq("hex alphabet", sha256_64_hex(hexish), sha256_hex(hexish));
  }

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
    const std::string kSeed05{obsidio::risk_hash("0.5")};
    const std::string kSeedNone{obsidio::risk_hash("none")};
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

  std::printf("\nRisk chain, x4 interleaved (what the pool runs at peak)\n");
  {
    const std::string kSeed05{obsidio::risk_hash("0.5")};
    const std::string kSeedNone{obsidio::risk_hash("none")};
    const std::string kSeed4821{obsidio::risk_hash("0.4821")};
    const std::string kSeedPi{obsidio::risk_hash("0.31415")};
    std::string a, b, c, d;

    obsidio::risk_hash_x4("0.5", "none", "0.4821", "0.31415", a, b, c, d);
    expect_eq("x4 lane A  seed=0.5", a, kSeed05);
    expect_eq("x4 lane B  seed=none", b, kSeedNone);
    expect_eq("x4 lane C  seed=0.4821", c, kSeed4821);
    expect_eq("x4 lane D  seed=0.31415", d, kSeedPi);

    obsidio::risk_hash_x4("0.31415", "0.4821", "none", "0.5", a, b, c, d);
    expect_eq("x4 rotated A  seed=0.31415", a, kSeedPi);
    expect_eq("x4 rotated B  seed=0.4821", b, kSeed4821);
    expect_eq("x4 rotated C  seed=none", c, kSeedNone);
    expect_eq("x4 rotated D  seed=0.5", d, kSeed05);

    obsidio::risk_hash_x4("0.5", "0.5", "0.5", "0.5", a, b, c, d);
    expect_eq("x4 same seed all lanes A==B", a, b);
    expect_eq("x4 same seed all lanes B==C", b, c);
    expect_eq("x4 same seed all lanes C==D", c, d);

    // Every batch size must agree, or the digest depends on queue depth.
    std::string ya, yb, yc;
    obsidio::risk_hash_x3("0.5", "none", "0.4821", ya, yb, yc);
    obsidio::risk_hash_x4("0.5", "none", "0.4821", "0.31415", a, b, c, d);
    expect_eq("x4 agrees with x3 lane A", a, ya);
    expect_eq("x4 agrees with x3 lane B", b, yb);
    expect_eq("x4 agrees with x3 lane C", c, yc);

    obsidio::risk_hash_x4("0.5", "0.4821", "none", "0.31415", a, b, c, d, 10);
    expect_eq("x4 n=10 lane A", a, obsidio::risk_hash("0.5", 10));
    expect_eq("x4 n=10 lane B", b, obsidio::risk_hash("0.4821", 10));
    expect_eq("x4 n=10 lane C", c, obsidio::risk_hash("none", 10));
    expect_eq("x4 n=10 lane D", d, obsidio::risk_hash("0.31415", 10));
    obsidio::risk_hash_x4("0.5", "none", "0.4821", "0.31415", a, b, c, d, 1);
    expect_eq("x4 n=1 lane A", a, obsidio::risk_hash("0.5", 1));
    expect_eq("x4 n=1 lane D", d, obsidio::risk_hash("0.31415", 1));
  }

  std::printf("\nRisk chain, x8 pipelined (the widest group the pool assembles)\n");
  {
    // x8 is a different kernel shape on x86, not a wider chain4: the schedule
    // is phase-split into an L1 buffer and the two phases are pipelined across
    // two groups of four. Two failure modes are unique to that -- a lane index
    // slipping between the A and B halves, and a fencepost in the group whose
    // schedule is primed before the loop -- so both get their own check.
    const char* seeds[8]{"0.5",     "none",    "0.4821",  "0.31415",
                            "1.61803", "2.71828", "",        "0.000001"};
    std::string s[8], got[8], want[8];
    for (int i{}; i < 8; ++i) {
      s[i] = seeds[i];
      want[i] = obsidio::risk_hash(seeds[i]);
    }

    obsidio::risk_hash_x8(s, got);
    for (int i{}; i < 8; ++i) {
      expect_eq(std::string("x8 lane ") + static_cast<char>('A' + i), got[i],
                want[i]);
    }

    // Reversed, so a lane that quietly reads its neighbour's slot moves.
    std::string rev[8], rgot[8];
    for (int i{}; i < 8; ++i) rev[i] = s[7 - i];
    obsidio::risk_hash_x8(rev, rgot);
    for (int i{}; i < 8; ++i) {
      expect_eq(std::string("x8 reversed lane ") + static_cast<char>('A' + i),
                rgot[i], want[7 - i]);
    }

    // Identical seeds: catches a lane that is computing the right answer for
    // the wrong input, which the checks above would not distinguish from a
    // correct one if the kernel simply broadcast lane 0.
    std::string same[8], sgot[8];
    for (int i{}; i < 8; ++i) same[i] = "0.5";
    obsidio::risk_hash_x8(same, sgot);
    for (int i{}; i < 8; ++i) {
      expect_eq(std::string("x8 same seed lane ") + static_cast<char>('A' + i),
                sgot[i], want[0]);
    }

    // Batch width must not change the digest, or the answer depends on how
    // deep the queue happened to be.
    std::string qa, qb, qc, qd;
    obsidio::risk_hash_x4(s[0], s[1], s[2], s[3], qa, qb, qc, qd);
    expect_eq("x8 agrees with x4 lane A", got[0], qa);
    expect_eq("x8 agrees with x4 lane D", got[3], qd);

    // Short chains, where the primed first schedule is most of the work.
    for (const int n : {1, 2, 3, 10}) {
      std::string ngot[8];
      obsidio::risk_hash_x8(s, ngot, n);
      for (int i{}; i < 8; ++i) {
        expect_eq(std::string("x8 n=") + std::to_string(n) + " lane " +
                      static_cast<char>('A' + i),
                  ngot[i], obsidio::risk_hash(seeds[i], n));
      }
    }
  }

  // -------------------------------------------------------------------------
  // Back end status, read differently depending on how this run was invoked.
  // Unforced: a rejection still serves correct digests, but costs ~7x, so on
  // an architecture that has a back end it fails the build rather than ship
  // quietly slow. Forced (the Dockerfile's passes): a CPU without the back end
  // SKIPs, since one image must build on either architecture, but a CPU that
  // has it and fails verification is the bug these passes exist to catch.
  std::printf("\nHash back end\n");
  std::printf("  selected: %s\n", obsidio::risk_backend_name());
  const char* forced{obsidio::risk_backend_forced()};
  const bool accelerated_forced =
      forced != nullptr &&
      (std::strcmp(forced, "arm") == 0 || std::strcmp(forced, "x86-sha-ni") == 0);

  if (obsidio::risk_backend_rejected()) {
    std::printf(
        "  FAIL  an accelerated back end was available but FAILED "
        "self-verification\n");
    ++g_failures;
  } else if (accelerated_forced && obsidio::risk_backend_partial()) {
    // Under a forced accelerated pass, a dropped lane means that lane's code
    // is wrong on this CPU. The default pass degrades gracefully in production;
    // here it must be loud enough to break the build.
    std::printf(
        "  FAIL  forced back end verified its core but DROPPED a lane\n");
    ++g_failures;
  } else if (obsidio::risk_backend_partial()) {
    std::printf(
        "  WARN  accelerated core verified but a wide lane was disabled; "
        "pool degraded to fewer lanes\n");
  } else if (accelerated_forced && std::strstr(obsidio::risk_backend_name(),
                                               "reference") != nullptr) {
    std::printf("  SKIP  forced back end \"%s\" not available on this CPU\n",
                forced);
    std::printf("\nskipped (forced back end unavailable)\n");
    return 0;
  }

  if (g_failures == 0) {
    std::printf("\nall checks passed\n");
    return 0;
  }
  std::printf("\n%d check(s) FAILED\n", g_failures);
  return 1;
}
