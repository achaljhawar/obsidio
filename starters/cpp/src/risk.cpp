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
//
// Verification is per entry point, not all-or-nothing. chain1/chain2 are the
// core: they were the paths verified on real silicon, and without them the
// back end has nothing to offer, so a core failure rejects the whole thing and
// we serve from the fast scalar fallback instead. chain3/chain4 reuse the same
// verified instruction sequences and differ only in scheduling order, but if
// either ever disagreed with the oracle -- on any future hardware, after any
// future edit -- nulling that single pointer degrades the pool to fewer lanes
// while keeping the accelerated core. A whole-back-end rejection here would
// cost ~7x throughput over a one-lane drop.
bool verify_core(const chain::Backend& b) {
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

// The third lane is the one most likely to be dropped or aliased by a
// copy-paste slip, so it gets distinct seeds of its own.
bool verify_lane3(const chain::Backend& b) {
  char ha[kSha256HexBytes], hb[kSha256HexBytes], hc[kSha256HexBytes];
  char oa[kSha256HexBytes], ob[kSha256HexBytes], oc[kSha256HexBytes];
  first_round("0.5", ha);
  first_round("0.9999", hb);
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
  return true;
}

// And lane four, with a fourth distinct seed.
bool verify_lane4(const chain::Backend& b) {
  char ha[kSha256HexBytes], hb[kSha256HexBytes], hc[kSha256HexBytes],
      hd[kSha256HexBytes];
  char oa[kSha256HexBytes], ob[kSha256HexBytes], oc[kSha256HexBytes],
      od[kSha256HexBytes];
  first_round("0.5", ha);
  first_round("0.9999", hb);
  first_round("0.31415", hc);
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

chain::Backend g_storage;
const chain::Backend* g_backend = nullptr;
bool g_rejected = false;
bool g_partial = false;
const char* g_forced = nullptr;
std::once_flag g_select_once;

void select_backend() {
  // RISK_BACKEND forces a specific path, so accelerated and reference builds
  // can be A/B'd on the same box, same binary, same run -- the only honest way
  // to quote a speedup. "reference" serves from the scalar fallback; "arm" and
  // "x86-sha-ni" each try exactly one accelerated back end, which is what the
  // Dockerfile's forced self-test passes use to exercise a specific one on its
  // real target hardware. Unset selects automatically: ARM crypto first, then
  // x86 SHA-NI.
  const char* forced = std::getenv("RISK_BACKEND");
  const chain::Backend* candidate = nullptr;
  if (forced != nullptr && std::strcmp(forced, "reference") == 0) {
    g_forced = forced;
    return;
  }
  if (forced == nullptr || std::strcmp(forced, "arm") == 0) {
    candidate = chain::arm_crypto_backend();
  }
  if (candidate == nullptr &&
      (forced == nullptr || std::strcmp(forced, "x86-sha-ni") == 0)) {
    candidate = chain::x86_sha_backend();
  }
  if (forced != nullptr && forced[0] != '\0') g_forced = forced;
  if (candidate == nullptr) return;  // no accelerated back end for this CPU

  if (!verify_core(*candidate)) {
    // Correctness wins: fall back rather than serve fast wrong digests.
    g_rejected = true;
    return;
  }

  g_storage = *candidate;
  g_backend = &g_storage;
  if (!verify_lane3(g_storage)) {
    g_storage.chain3 = nullptr;
    g_partial = true;
  }
  if (!verify_lane4(g_storage)) {
    g_storage.chain4 = nullptr;
    g_partial = true;
  }
}

}  // namespace

void init_risk_backend() { std::call_once(g_select_once, select_backend); }

int risk_lane_width() {
  init_risk_backend();
  if (g_backend == nullptr) return 1;
  int width = g_backend->lanes;
  // A lane that failed verification was nulled out above, and the risk_hash_xN
  // wrappers quietly compose around the hole. Narrow the batch instead, so the
  // pool stops assembling groups the back end can no longer run as one.
  if (width >= 4 && g_backend->chain4 == nullptr) width = 3;
  if (width >= 3 && g_backend->chain3 == nullptr) width = 2;
  if (width < 1) width = 1;
  return width;
}

const char* risk_backend_name() {
  init_risk_backend();
  if (g_backend != nullptr) {
    if (g_partial) return "accelerated (some lanes degraded to fewer)";
    return g_backend->name;
  }
  return g_rejected ? "reference (accelerated back end REJECTED)"
                    : "reference (specialised 64-byte transform)";
}

bool risk_backend_rejected() {
  init_risk_backend();
  return g_rejected;
}

bool risk_backend_partial() {
  init_risk_backend();
  return g_partial;
}

// The RISK_BACKEND value this process was started with, or nullptr. The
// self-test uses it to distinguish "forced back end absent on this CPU, skip"
// from a real verification failure.
const char* risk_backend_forced() {
  init_risk_backend();
  return g_forced;
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
  if (iterations <= 0 || g_backend == nullptr ||
      g_backend->chain3 == nullptr) {
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
  if (iterations <= 0 || g_backend == nullptr ||
      g_backend->chain4 == nullptr) {
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
