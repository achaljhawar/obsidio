// Portable SHA-256: the reference implementation the self-test checks
// accelerated back ends against. Keep it plain and correct.
#pragma once

#include <cstddef>
#include <cstdint>

namespace obsidio {

constexpr std::size_t kSha256DigestBytes{32};
constexpr std::size_t kSha256HexBytes{64};

// Hash `len` bytes at `data` into the 32-byte buffer `out`. Generic path.
void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[32]);

// Hash exactly 64 bytes; skips the generic padding path since the second block
// is a compile-time constant. Must match sha256(in, 64, out) byte for byte.
void sha256_64(const std::uint8_t in[64], std::uint8_t out[32]);

// Lowercase hex-encode `in_len` bytes into `out` (2*in_len bytes written).
// Does not null-terminate.
void hex_encode(const std::uint8_t* in, std::size_t in_len, char* out);

}  // namespace obsidio
