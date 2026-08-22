// Pluggable back ends for the /risk hash chain. A back end only handles the
// steady state (64 hex chars in, 64 out); round one stays on the portable
// path. risk.cpp will not select one until it reproduces reference digests.
#pragma once

namespace obsidio {
namespace chain {

struct Backend {
  const char* name;

  // How many chains this back end genuinely advances in lockstep -- NOT the
  // widest chainN it exposes. The pool batches to this; composing wider calls
  // from narrower kernels must never leave an odd chain running alone (x3 on a
  // width-2 back end measured 39% worse than x2). Do not hardcode: ARM is a
  // real 4-lane interleave, x86 is 8 via the phase-split chain8 below.
  int lanes;

  // Advance one chain `rounds` more times from a 64-hex-char state.
  // `rounds == 0` copies `in` to `out`. `in` and `out` may not overlap.
  void (*chain1)(const char in[64], int rounds, char out[64]);

  // Two independent chains in lockstep. Same contract as chain1.
  void (*chain2)(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]);

  // Three in lockstep. Same contract.
  void (*chain3)(const char in_a[64], const char in_b[64], const char in_c[64],
                 int rounds, char out_a[64], char out_b[64], char out_c[64]);

  // Four in lockstep. Same contract.
  void (*chain4)(const char in_a[64], const char in_b[64], const char in_c[64],
                 const char in_d[64], int rounds, char out_a[64],
                 char out_b[64], char out_c[64], char out_d[64]);

  // Eight in lockstep, or nullptr when there is no eight-wide path. On x86
  // this is a structurally different kernel: schedule phase-split into an L1
  // buffer, phases pipelined across lane groups (see chain_x86.cpp).
  void (*chain8)(const char in[8][64], int rounds, char out[8][64]);
};

// nullptr unless built for aarch64 and the CPU advertises the SHA-2 extension.
const Backend* arm_crypto_backend();

// nullptr unless built for x86-64 and the CPU advertises SHA-NI + SSSE3/SSE4.1.
const Backend* x86_sha_backend();

}  // namespace chain
}  // namespace obsidio
