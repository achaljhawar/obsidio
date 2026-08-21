#include "risk.hpp"

#include <cstdint>
#include <cstring>

#include "sha256.hpp"

namespace obsidio {

std::string risk_hash(const std::string& seed, int iterations) {
  if (iterations <= 0) return seed;

  // Two fixed 64-byte buffers, reused for every iteration. After the first
  // round the value being hashed is always exactly 64 hex chars, so nothing in
  // this loop allocates.
  //
  // (The Go and Node starters allocate a fresh string per iteration -- 100k
  // allocations per request. Not doing that is the first easy win, and it is
  // already done here.)
  char hex[kSha256HexBytes];
  std::uint8_t digest[kSha256DigestBytes];

  // First round works on the caller's seed, whatever length it is.
  sha256(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size(), digest);
  hex_encode(digest, kSha256DigestBytes, hex);

  // Every remaining round hashes exactly 64 bytes.
  //
  // OPTIMISATION HOOK: because the input is always 64 bytes, each of these
  // hashes is exactly two SHA-256 blocks and the SECOND block is a compile-time
  // constant (0x80, zeros, length = 512 bits). A specialised two-block
  // transform that skips the generic padding path -- and, better, one built on
  // SHA-NI / ARMv8 crypto intrinsics with runtime dispatch -- belongs right
  // here. Keep the portable path above as the fallback.
  for (int i = 1; i < iterations; ++i) {
    sha256(reinterpret_cast<const std::uint8_t*>(hex), kSha256HexBytes, digest);
    hex_encode(digest, kSha256DigestBytes, hex);
  }

  return std::string(hex, kSha256HexBytes);
}

}  // namespace obsidio
