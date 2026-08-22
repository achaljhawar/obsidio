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

#include "../src/chain_model_ops.hpp"
#include "../src/chain_x86_core.hpp"
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

std::string sha256_64_hex(const std::string& input) {
  std::uint8_t digest[obsidio::kSha256DigestBytes];
  obsidio::sha256_64(reinterpret_cast<const std::uint8_t*>(input.data()), digest);
  char hex[obsidio::kSha256HexBytes];
  obsidio::hex_encode(digest, obsidio::kSha256DigestBytes, hex);
  return std::string(hex, obsidio::kSha256HexBytes);
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

  // The specialised two-block transform must agree with the generic one on
  // every 64-byte input, since it is what the fallback chain actually runs.
  std::printf("\nSpecialised 64-byte transform (sha256_64)\n");
  expect_eq("64 x 'a'", sha256_64_hex(std::string(64, 'a')),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  expect_eq("64 x '\\0'", sha256_64_hex(std::string(64, '\0')),
            sha256_hex(std::string(64, '\0')));
  {
    // A real hex digest, which is the only input shape the chain ever feeds it.
    const std::string h = sha256_hex("0.5");
    expect_eq("hex digest of 0.5", sha256_64_hex(h), sha256_hex(h));
    std::string all_ff(64, 'f');
    expect_eq("64 x 'f'", sha256_64_hex(all_ff), sha256_hex(all_ff));
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

  // -------------------------------------------------------------------------
  // The fallback chain, checked against the SAME golden digests. On a machine
  // where an accelerated back end qualifies this path never runs in production,
  // so without an explicit check a bug in it would stay hidden until the one
  // day it matters -- an unknown grading CPU where the back end is rejected.
  std::printf("\nFallback chain (no accelerated back end)\n");
  expect_eq("reference seed=0.5", obsidio::risk_hash_reference("0.5"),
            "8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148");
  expect_eq("reference seed=none", obsidio::risk_hash_reference("none"),
            "5b83a4893dc72cfd45dabf4fae920510e38954cafa2481efce3f7815d09bc460");
  expect_eq("reference seed=0.4821 n=10",
            obsidio::risk_hash_reference("0.4821", 10),
            "39cccfc2d94087e4df1dccf10ee55d9b7221d48919e065cad1ee933b5fd546c4");

  // -------------------------------------------------------------------------
  // The x86 SHA-NI algorithm, run under a portable model of every instruction
  // it uses. This is the only check on that code that can run on a machine
  // without SHA-NI silicon, and it runs on EVERY architecture -- so the x86
  // back end cannot rot unnoticed while we develop on ARM.
  //
  // SHA-256 is unforgiving: any misunderstanding of the round or message
  // schedule semantics produces garbage, not a near miss. Passing here means
  // the algorithm structure is right.
  std::printf("\nx86 SHA-NI algorithm under instruction model\n");
  {
    using obsidio::chain::ModelOps;
    // The chain state after round one, i.e. hex(sha256(seed)).
    const char* h_05 =
        "d2cbad71ff333de67d07ec676e352ab7f38248eb69c942950157220607c55e84";
    const char* h_none =
        "140bedbf9c3f6d56a9846d2ba7088798683f4da0c248231336e6a05679e4fdfe";
    char a[64], b[64];

    obsidio::chain::chain1_core<ModelOps>(h_05, 0, a);
    expect_eq("model rounds=0 is identity", std::string(a, 64), h_05);

    obsidio::chain::chain1_core<ModelOps>(h_05, 9, a);
    expect_eq("model seed=0.5 n=10", std::string(a, 64),
              "0d7d5afa9207bab8203501c8a7505d8e13b0ada58ec966c3f9b1c794e59308c5");

    obsidio::chain::chain1_core<ModelOps>(h_05, obsidio::kRiskIterations - 1, a);
    expect_eq("model seed=0.5 n=50000 (graded path)", std::string(a, 64),
              "8dc4014994d6d0df04656cb1d5988562af06015babd9592bf37451173c451148");

    // Both lanes of the interleaved path, with different seeds, to catch a
    // lane-crossing bug in the x86 core specifically.
    obsidio::chain::chain2_core<ModelOps>(h_05, h_none, 9, a, b);
    expect_eq("model x2 lane A n=10", std::string(a, 64),
              "0d7d5afa9207bab8203501c8a7505d8e13b0ada58ec966c3f9b1c794e59308c5");
    expect_eq("model x2 lane B n=10", std::string(b, 64),
              "c15c75ca43ca0c34a553598c11847525bf458005611493c6c795e5dbd401ee57");
    obsidio::chain::chain2_core<ModelOps>(h_none, h_05, 9, a, b);
    expect_eq("model x2 swapped lane A", std::string(a, 64),
              "c15c75ca43ca0c34a553598c11847525bf458005611493c6c795e5dbd401ee57");
    expect_eq("model x2 swapped lane B", std::string(b, 64),
              "0d7d5afa9207bab8203501c8a7505d8e13b0ada58ec966c3f9b1c794e59308c5");
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
