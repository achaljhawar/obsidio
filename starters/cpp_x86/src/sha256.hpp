// Portable SHA-256. Deliberately plain: this is the reference implementation
// the self-test checks against, and the baseline you replace when you start
// optimising (SHA-NI / ARMv8 crypto intrinsics, multi-buffer, etc).
//
// Keep this file working and correct. Optimise in risk.cpp instead, and keep
// the self-test passing against these primitives.
#pragma once

#include <cstddef>
#include <cstdint>

namespace obsidio {

constexpr std::size_t kSha256DigestBytes = 32;
constexpr std::size_t kSha256HexBytes = 64;

// Hash `len` bytes at `data` into the 32-byte buffer `out`.
void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[32]);

// SHA-256 of EXACTLY 64 bytes -- the shape every /risk iteration after the
// first has, since the input is always a 64-char hex digest.
//
// Skips everything the generic one-shot path pays for: no context zeroing, no
// memcpy into a buffer, no length bookkeeping, no runtime padding. A 64-byte
// message is exactly two blocks and the second is a compile-time constant
// (0x80, zeros, bit length 512), so this is two block transforms and a store.
//
// This is what the fallback chain runs. It matters on any machine where the
// accelerated back end does not qualify -- which is every machine whose
// architecture we have not written a back end for.
void sha256_64(const std::uint8_t in[64], std::uint8_t out[32]);

// Lowercase hex-encode `in_len` bytes into `out`. Writes 2*in_len bytes.
// Does NOT null-terminate.
void hex_encode(const std::uint8_t* in, std::size_t in_len, char* out);

}  // namespace obsidio
