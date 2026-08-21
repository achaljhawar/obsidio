#include "risk.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "chain_backend.hpp"
#include "sha256.hpp"

namespace obsidio {
namespace {

// The reference chain: one library call per iteration. Kept verbatim as the
// thing every accelerated back end is checked against, and used as the fallback
// when no back end qualifies.
std::string chain_reference(const std::string& seed, int iterations) {
  if (iterations <= 0) return seed;

  char hex[kSha256HexBytes];
  std::uint8_t digest[kSha256DigestBytes];

  sha256(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size(), digest);
  hex_encode(digest, kSha256DigestBytes, hex);

  for (int i = 1; i < iterations; ++i) {
    sha256(reinterpret_cast<const std::uint8_t*>(hex), kSha256HexBytes, digest);
    hex_encode(digest, kSha256DigestBytes, hex);
  }
  return std::string(hex, kSha256HexBytes);
}

// Round one, the only hash whose input is not exactly 64 bytes. Back ends start
// from its output, so none of them needs a generic padding path.
void first_round(const std::string& seed, char out[kSha256HexBytes]) {
  std::uint8_t digest[kSha256DigestBytes];
  sha256(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size(), digest);
  hex_encode(digest, kSha256DigestBytes, out);
}

// A back end is only trusted if it reproduces the reference digest, here, at
// startup. A wrong digest is worth zero and fails silently -- it still looks
// like 64 plausible hex characters -- so the cost of being wrong is the whole
// score. Verification costs microseconds; it is not optional.
bool verify_backend(const chain::Backend& b) {
  // Seeds chosen to cover what the endpoint can actually receive: a normal k6
  // seed, the empty-seed sentinel, an odd length, and the empty string.
  const char* seeds[] = {"0.5", "none", "0.4821", "", "0.123456789012345"};

  for (const char* s : seeds) {
    char h0[kSha256HexBytes];
    first_round(s, h0);

    // rounds == 0 must be the identity, and short chains must match exactly.
    for (const int iterations : {1, 2, 3, 4, 17, 65}) {
      char got[kSha256HexBytes];
      b.chain1(h0, iterations - 1, got);
      if (chain_reference(s, iterations) != std::string(got, kSha256HexBytes)) {
        return false;
      }
    }
  }

  // chain2 must agree with chain1 -- an interleaving bug that crosses the two
  // lanes would otherwise be invisible until it shipped.
  char ha[kSha256HexBytes], hb[kSha256HexBytes];
  char oa[kSha256HexBytes], ob[kSha256HexBytes];
  first_round("0.5", ha);
  first_round("0.9999", hb);
  for (const int iterations : {1, 2, 12, 33}) {
    b.chain2(ha, hb, iterations - 1, oa, ob);
    if (chain_reference("0.5", iterations) != std::string(oa, kSha256HexBytes)) {
      return false;
    }
    if (chain_reference("0.9999", iterations) != std::string(ob, kSha256HexBytes)) {
      return false;
    }
  }
  return true;
}

const chain::Backend* g_backend = nullptr;
bool g_rejected = false;
std::once_flag g_select_once;

void select_backend() {
  // RISK_BACKEND=reference forces the slow path. This exists so the accelerated
  // and reference builds can be A/B'd on the same box, same binary, same run --
  // which is the only honest way to quote a speedup.
  const char* forced = std::getenv("RISK_BACKEND");
  if (forced != nullptr && std::strcmp(forced, "reference") == 0) return;

  // Both back ends are now auto-selected: each has been run and measured on
  // its real target hardware. ARM: STRATEGY.md section 5 (Apple Silicon).
  // x86 SHA-NI: verified 2026-08-22 on a real x86-64 host under WSL2 --
  // tests/selftest.cpp passed all 21 digest checks (FIPS vectors, short
  // chains, full 50,000-round chains, and every x2-interleaved variant), then
  // measured at ~4.0x the OpenSSL-per-call reference path (16.7ms -> 4.2ms
  // per chain, three repeated trials, same binary, same run). See
  // STRATEGY.md section 12 for the full note. verify_backend() below is
  // still the final gate either way, on every boot, on every architecture --
  // this is defense in depth, not a reason to skip re-verifying after any
  // future change to chain_x86.cpp.
  const chain::Backend* candidate = chain::arm_crypto_backend();
  if (candidate == nullptr) candidate = chain::x86_sha_backend();
  if (candidate == nullptr) return;  // no accelerated back end for this CPU
  if (verify_backend(*candidate)) {
    g_backend = candidate;
  } else {
    // Correctness wins: fall back rather than serve fast wrong digests. The
    // self-test turns this into a build failure so it can never ship unnoticed.
    g_rejected = true;
  }
}

}  // namespace

void init_risk_backend() { std::call_once(g_select_once, select_backend); }

const char* risk_backend_name() {
  init_risk_backend();
  if (g_backend != nullptr) return g_backend->name;
  return g_rejected ? "reference (accelerated back end REJECTED)"
                    : "reference (per-call SHA-256)";
}

bool risk_backend_rejected() {
  init_risk_backend();
  return g_rejected;
}

std::string risk_hash(const std::string& seed, int iterations) {
  if (iterations <= 0) return seed;
  init_risk_backend();
  if (g_backend == nullptr) return chain_reference(seed, iterations);

  char state[kSha256HexBytes];
  char out[kSha256HexBytes];
  first_round(seed, state);
  g_backend->chain1(state, iterations - 1, out);
  return std::string(out, kSha256HexBytes);
}

void risk_hash_x2(const std::string& seed_a, const std::string& seed_b,
                  std::string& out_a, std::string& out_b, int iterations) {
  init_risk_backend();
  if (g_backend == nullptr || iterations <= 0) {
    // Correct either way, just without the interleaving win.
    out_a = risk_hash(seed_a, iterations);
    out_b = risk_hash(seed_b, iterations);
    return;
  }

  char sa[kSha256HexBytes], sb[kSha256HexBytes];
  char ra[kSha256HexBytes], rb[kSha256HexBytes];
  first_round(seed_a, sa);
  first_round(seed_b, sb);
  g_backend->chain2(sa, sb, iterations - 1, ra, rb);
  out_a.assign(ra, kSha256HexBytes);
  out_b.assign(rb, kSha256HexBytes);
}

}  // namespace obsidio
