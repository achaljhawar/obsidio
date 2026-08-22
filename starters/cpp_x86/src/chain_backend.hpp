// Pluggable back ends for the /risk hash chain.
//
// A back end only ever handles the STEADY STATE of the chain: after the first
// hash, the value being hashed is always exactly 64 lowercase hex characters,
// so every message is two SHA-256 blocks and nothing ever needs the generic
// padding path. Round one (arbitrary-length seed) stays in risk.cpp on the
// portable path, so a back end never has to care about seed length.
//
// Two entry points, because a single chain is latency-bound: `sha256h` has
// roughly 4-cycle latency but ~1-cycle throughput, so one serial chain leaves
// most of the crypto pipeline idle. `chain2` advances two independent requests
// in lockstep and recovers that. Measured on an M2: 2.97 -> 2.03 ms/chain.
//
// Adding a back end (e.g. x86 SHA-NI) means writing one of these structs and
// returning it from a `*_backend()` function below. risk.cpp will not select
// it until it reproduces the reference digests -- see verify_backend().
#pragma once

namespace obsidio {
namespace chain {

struct Backend {
  const char* name;

  // Advance one chain `rounds` more times from a 64-hex-char state.
  // `rounds == 0` copies `in` to `out`. `in` and `out` may not overlap.
  void (*chain1)(const char in[64], int rounds, char out[64]);

  // Advance two independent chains in lockstep. Same contract as chain1.
  void (*chain2)(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]);
};

// Returns nullptr when this build was not compiled for aarch64, or when the
// CPU does not advertise the ARMv8 SHA-2 extension.
const Backend* arm_crypto_backend();

// Returns nullptr when this build was not compiled for x86, or when the CPU
// does not advertise SHA-NI + SSSE3 + SSE4.1.
const Backend* x86_shani_backend();

}  // namespace chain
}  // namespace obsidio
