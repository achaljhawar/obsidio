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

  // How many chains this back end genuinely advances in lockstep, which is NOT
  // the same as the widest chainN it exposes. chain3/chain4 are always callable
  // and always correct, but a back end may implement them by composition rather
  // than by interleaving: x86's chain4 is two sequential chain2 calls and its
  // chain3 is chain2 + chain1, because six XMM registers per lane makes two
  // lanes the register ceiling there. ARM's chain4 is a real four-lane
  // interleave.
  //
  // The pool batches to this, so it never assembles a group whose odd chain
  // would fall back to the 1-lane rate. Measured on a Ryzen 7 170: chain3 runs
  // at 390.84 chains/s against chain2's 640.46 -- a three-job batch is 39%
  // WORSE than a two-job batch, because the third lane runs alone.
  //
  // Do not "simplify" this to a constant 2. Capping ARM at two lanes would
  // halve its instruction-level parallelism.
  //
  // x86 is 8 now, not because it grew six more register-resident lanes but
  // because chain8 is a structurally different kernel -- see chain8 below. Its
  // sub-eight compositions are still pairs, and the pool knows that: below a
  // full group it batches exactly as it did when this said 2.
  int lanes;

  // Advance one chain `rounds` more times from a 64-hex-char state.
  // `rounds == 0` copies `in` to `out`. `in` and `out` may not overlap.
  void (*chain1)(const char in[64], int rounds, char out[64]);

  // Advance two independent chains in lockstep. Same contract as chain1.
  void (*chain2)(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]);

  // Three in lockstep. Same contract again.
  void (*chain3)(const char in_a[64], const char in_b[64], const char in_c[64],
                 int rounds, char out_a[64], char out_b[64], char out_c[64]);

  // Four in lockstep. A Lane is six 128-bit vectors, so four lanes want 24 of
  // the 32 NEON registers -- the point where the register file starts to bind.
  // Whether this beats chain3 is a measurement, not a prediction: the isolated
  // chain benchmark called x3 and x4 a wash, but the pool-level batching effect
  // it cannot see is what actually paid off going from two to three.
  void (*chain4)(const char in_a[64], const char in_b[64], const char in_c[64],
                 const char in_d[64], int rounds, char out_a[64],
                 char out_b[64], char out_c[64], char out_d[64]);

  // Eight in lockstep, or nullptr when this back end has no eight-wide path.
  // Arrays rather than sixteen loose pointers, purely for legibility.
  //
  // This is a different shape of kernel from chain1..chain4, not just a wider
  // one. Those hold the message schedule in registers alongside the chain
  // state, which is what caps x86 at two genuine lanes. The eight-wide x86
  // path phase-splits -- schedule into an L1 buffer, then rounds at 2
  // registers per lane -- and pipelines the two phases across lane groups so
  // the schedule still hides under the rounds. See chain_x86.cpp.
  //
  // nullptr is the normal case: ARM's four-lane interleave is already a good
  // use of a 32-entry register file and was not re-measured in this shape.
  void (*chain8)(const char in[8][64], int rounds, char out[8][64]);
};

// Returns nullptr when this build was not compiled for aarch64, or when the
// CPU does not advertise the ARMv8 SHA-2 extension.
const Backend* arm_crypto_backend();

// Returns nullptr when this build was not compiled for x86-64, or when the
// CPU does not advertise SHA-NI (plus SSSE3/SSE4.1).
const Backend* x86_sha_backend();

}  // namespace chain
}  // namespace obsidio
