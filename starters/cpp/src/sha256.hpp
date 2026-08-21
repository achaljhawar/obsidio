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

// Hash `len` bytes at `data` into the 32-byte buffer `out`. Generic path.
void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[32]);

// Hash exactly 64 bytes. A 64-byte message is always exactly two SHA-256
// blocks and the second one is a compile-time constant, so this skips the
// generic length/padding path and -- on the OpenSSL build -- the per-call
// provider fetch that dominates a one-shot SHA256() of a short input.
//
// This is the hot path of the reference risk chain: every round after the
// first hashes exactly 64 hex chars. The accelerated back ends in
// chain_backend.hpp do the same specialisation internally and do not call it.
//
// Must produce byte-identical output to sha256(in, 64, out); the self-test
// asserts that on both backends.
void sha256_64(const std::uint8_t in[64], std::uint8_t out[32]);

// Lowercase hex-encode `in_len` bytes into `out`. Writes 2*in_len bytes.
// Does NOT null-terminate.
void hex_encode(const std::uint8_t* in, std::size_t in_len, char* out);

}  // namespace obsidio
