#include "risk.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "chain_backend.hpp"
#include "sha256.hpp"

namespace obsidio {
namespace {

// The oracle every accelerated back end is checked against: one generic library
// call per round, deliberately the plainest expression of the spec. Never
// serves a request, so leave it slow and obviously correct.
std::string chain_reference(const std::string& seed, int iterations) {
  if (iterations <= 0) return seed;

  char hex[kSha256HexBytes];
  std::uint8_t digest[kSha256DigestBytes];

  sha256(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size(), digest);
  hex_encode(digest, kSha256DigestBytes, hex);

  for (int i{1}; i < iterations; ++i) {
    sha256(reinterpret_cast<const std::uint8_t*>(hex), kSha256HexBytes, digest);
    hex_encode(digest, kSha256DigestBytes, hex);
  }
  return std::string(hex, kSha256HexBytes);
}

// The serving fallback when no accelerated back end qualifies. Same digests as
// chain_reference but through the specialised 64-byte transform, roughly 5x.
//
// Kept separate from the oracle on purpose: sharing this primitive would let a
// bug in sha256_64 move both and verify a broken back end.
std::string chain_fallback(const std::string& seed, int iterations) {
  if (iterations <= 0) return seed;

  char hex[kSha256HexBytes];
  std::uint8_t digest[kSha256DigestBytes];

  sha256(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size(), digest);
  hex_encode(digest, kSha256DigestBytes, hex);

  for (int i{1}; i < iterations; ++i) {
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

// A wrong digest is worth zero and still looks like 64 plausible hex chars, so
// nothing serves until it reproduces the oracle. Verification is per entry
// point: a core failure rejects the back end, while a wider lane that fails is
// nulled individually and the pool degrades rather than losing ~7x throughput.
bool verify_core(const chain::Backend& b) {
  // Seeds cover what the endpoint can actually receive: a k6 seed, the
  // empty-seed sentinel, an odd length, and the empty string.
  const char* seeds[]{"0.5", "none", "0.4821", "", "0.123456789012345"};

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

  // chain2 must agree with chain1, or a cross-lane interleaving bug stays
  // invisible until it ships.
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

// The third lane is the one a copy-paste slip most easily drops or aliases, so
// it gets a distinct seed of its own.
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

// chain8 is a different kind of kernel, not a wider copy, so its bugs do not
// look like the narrow lanes' bugs. Round counts 1-3 matter: the pipeline
// primes one group's schedule before the loop, and a fencepost there would
// only surface on the shortest chains.
bool verify_lane8(const chain::Backend& b) {
  const char* seeds[8]{"0.5",     "0.9999",  "0.31415", "0.271828",
                       "1.61803", "2.71828", "",        "0.000001"};
  char h[8][kSha256HexBytes];
  char o[8][kSha256HexBytes];
  for (int i{}; i < 8; ++i) first_round(seeds[i], h[i]);

  for (const int iterations : {1, 2, 3, 4, 12, 33}) {
    b.chain8(h, iterations - 1, o);
    for (int i{}; i < 8; ++i) {
      if (chain_reference(seeds[i], iterations) !=
          std::string(o[i], kSha256HexBytes)) {
        return false;
      }
    }
  }

  // Near-identical seeds catch a group-index slip between the A and B halves,
  // which the distinct-seed pass above would miss if lane 0 were copied.
  const char* twins[8]{"0.10", "0.11", "0.12", "0.13",
                       "0.14", "0.15", "0.16", "0.17"};
  for (int i{}; i < 8; ++i) first_round(twins[i], h[i]);
  b.chain8(h, 16, o);
  for (int i{}; i < 8; ++i) {
    if (chain_reference(twins[i], 17) != std::string(o[i], kSha256HexBytes)) {
      return false;
    }
  }
  return true;
}

chain::Backend g_storage;
const chain::Backend* g_backend{nullptr};
bool g_rejected{false};
bool g_partial{false};
const char* g_forced{nullptr};
std::once_flag g_select_once;

void select_backend() {
  // RISK_BACKEND forces one path so accelerated and reference can be A/B'd on
  // the same binary and run. Unset selects ARM crypto first, then x86 SHA-NI.
  const char* forced{std::getenv("RISK_BACKEND")};
  const chain::Backend* candidate{nullptr};
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
  if (g_storage.chain8 != nullptr && !verify_lane8(g_storage)) {
    g_storage.chain8 = nullptr;
    g_partial = true;
  }
}

}  // namespace

void init_risk_backend() { std::call_once(g_select_once, select_backend); }

int risk_lane_width() {
  init_risk_backend();
  if (g_backend == nullptr) return 1;
  int width{g_backend->lanes};
  // A nulled lane still composes correctly through the wrappers, but the pool
  // must stop assembling groups the back end can no longer run as one. Eight
  // degrades to four: chain8 is the only eight-wide path.
  if (width >= 8 && g_backend->chain8 == nullptr) width = 4;
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

void risk_hash_x8(const std::string seeds[8], std::string out[8],
                  int iterations) {
  init_risk_backend();
  if (iterations <= 0 || g_backend == nullptr ||
      g_backend->chain8 == nullptr) {
    // Two four-lane groups, so a back end without chain8 still gets its width.
    risk_hash_x4(seeds[0], seeds[1], seeds[2], seeds[3], out[0], out[1], out[2],
                 out[3], iterations);
    risk_hash_x4(seeds[4], seeds[5], seeds[6], seeds[7], out[4], out[5], out[6],
                 out[7], iterations);
    return;
  }

  char s[8][kSha256HexBytes];
  char r[8][kSha256HexBytes];
  for (int i{}; i < 8; ++i) first_round(seeds[i], s[i]);
  g_backend->chain8(s, iterations - 1, r);
  for (int i{}; i < 8; ++i) out[i].assign(r[i], kSha256HexBytes);
}

}  // namespace obsidio
