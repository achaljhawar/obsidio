#include "risk.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "chain_backend.hpp"
#include "sha256.hpp"

namespace obsidio {
namespace {

// The reference chain: one generic library call per iteration. This is the
// ORACLE -- the thing every accelerated back end is checked against in
// verify_backend() -- and it is deliberately the slowest, plainest expression
// of the spec. It is never used to serve a request.
//
// Do not "optimise" this. Its only job is to be obviously correct, and it costs
// nothing: verify_backend() runs it for at most 65 iterations, once, at start.
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

// The serving fallback, used when no accelerated back end qualifies: an x86
// grading box, an ARM core without HWCAP_SHA2, RISK_BACKEND=reference, or a
// back end that failed verification. Same digest as chain_reference(), but the
// 49,999 hot rounds go through the specialised 64-byte transform instead of the
// one-shot API and its per-call provider fetch. Roughly 5x.
//
// Kept SEPARATE from chain_reference() on purpose. If the oracle and the code
// it validates shared this primitive, a bug in sha256_64 would move both and
// verify_backend() could accept a broken back end. The oracle stays on the most
// exercised primitive in the tree; the self-test pins sha256_64 to it directly.
std::string chain_fallback(const std::string& seed, int iterations) {
  if (iterations <= 0) return seed;

  char hex[kSha256HexBytes];
  std::uint8_t digest[kSha256DigestBytes];

  sha256(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size(), digest);
  hex_encode(digest, kSha256DigestBytes, hex);

  for (int i = 1; i < iterations; ++i) {
    sha256_64(reinterpret_cast<const std::uint8_t*>(hex), digest);
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
  char ha[kSha256HexBytes], hb[kSha256HexBytes], hc[kSha256HexBytes];
  char oa[kSha256HexBytes], ob[kSha256HexBytes], oc[kSha256HexBytes];
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

  // Same again for chain3. The third lane is the one most likely to be dropped
  // or aliased by a copy-paste slip, so it gets a distinct seed of its own.
  first_round("0.31415", hc);
  for (const int iterations : {1, 2, 12, 33}) {
    b.chain3(ha, hb, hc, iterations - 1, oa, ob, oc);
    if (chain_reference("0.5", iterations) != std::string(oa, kSha256HexBytes)) {
      return false;
    }
    if (chain_reference("0.9999", iterations) != std::string(ob, kSha256HexBytes)) {
      return false;
    }
    if (chain_reference("0.31415", iterations) != std::string(oc, kSha256HexBytes)) {
      return false;
    }
  }

  // And chain4, with a fourth distinct seed.
  char hd[kSha256HexBytes], od[kSha256HexBytes];
  first_round("0.271828", hd);
  for (const int iterations : {1, 2, 12, 33}) {
    b.chain4(ha, hb, hc, hd, iterations - 1, oa, ob, oc, od);
    if (chain_reference("0.5", iterations) != std::string(oa, kSha256HexBytes)) {
      return false;
    }
    if (chain_reference("0.9999", iterations) != std::string(ob, kSha256HexBytes)) {
      return false;
    }
    if (chain_reference("0.31415", iterations) != std::string(oc, kSha256HexBytes)) {
      return false;
    }
    if (chain_reference("0.271828", iterations) != std::string(od, kSha256HexBytes)) {
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

  const chain::Backend* candidate = chain::arm_crypto_backend();
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
                    : "reference (specialised 64-byte transform)";
}

bool risk_backend_rejected() {
  init_risk_backend();
  return g_rejected;
}

std::string risk_hash(const std::string& seed, int iterations) {
  if (iterations <= 0) return seed;
  init_risk_backend();
  if (g_backend == nullptr) return chain_fallback(seed, iterations);

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

void risk_hash_x3(const std::string& seed_a, const std::string& seed_b,
                  const std::string& seed_c, std::string& out_a,
                  std::string& out_b, std::string& out_c, int iterations) {
  init_risk_backend();
  if (g_backend == nullptr || iterations <= 0) {
    // Correct either way, just without the interleaving win.
    out_a = risk_hash(seed_a, iterations);
    out_b = risk_hash(seed_b, iterations);
    out_c = risk_hash(seed_c, iterations);
    return;
  }

  char sa[kSha256HexBytes], sb[kSha256HexBytes], sc[kSha256HexBytes];
  char ra[kSha256HexBytes], rb[kSha256HexBytes], rc[kSha256HexBytes];
  first_round(seed_a, sa);
  first_round(seed_b, sb);
  first_round(seed_c, sc);
  g_backend->chain3(sa, sb, sc, iterations - 1, ra, rb, rc);
  out_a.assign(ra, kSha256HexBytes);
  out_b.assign(rb, kSha256HexBytes);
  out_c.assign(rc, kSha256HexBytes);
}

void risk_hash_x4(const std::string& seed_a, const std::string& seed_b,
                  const std::string& seed_c, const std::string& seed_d,
                  std::string& out_a, std::string& out_b, std::string& out_c,
                  std::string& out_d, int iterations) {
  init_risk_backend();
  if (g_backend == nullptr || iterations <= 0) {
    out_a = risk_hash(seed_a, iterations);
    out_b = risk_hash(seed_b, iterations);
    out_c = risk_hash(seed_c, iterations);
    out_d = risk_hash(seed_d, iterations);
    return;
  }

  char sa[kSha256HexBytes], sb[kSha256HexBytes];
  char sc[kSha256HexBytes], sd[kSha256HexBytes];
  char ra[kSha256HexBytes], rb[kSha256HexBytes];
  char rc[kSha256HexBytes], rd[kSha256HexBytes];
  first_round(seed_a, sa);
  first_round(seed_b, sb);
  first_round(seed_c, sc);
  first_round(seed_d, sd);
  g_backend->chain4(sa, sb, sc, sd, iterations - 1, ra, rb, rc, rd);
  out_a.assign(ra, kSha256HexBytes);
  out_b.assign(rb, kSha256HexBytes);
  out_c.assign(rc, kSha256HexBytes);
  out_d.assign(rd, kSha256HexBytes);
}

}  // namespace obsidio
