// x86 SHA-NI back end for the /risk chain.
//
// The algorithm is in chain_x86_core.hpp; this file only supplies the real
// intrinsics and the runtime CPU gate. On a non-x86 build it collapses to
// `return nullptr`.
//
// IMPORTANT, and stated plainly: this back end has never been executed on x86
// hardware. It was developed on arm64, where the only available x86 emulation
// (Rosetta) does not implement SHA-NI. What HAS been verified is:
//
//   * the algorithm, run under a portable model of every instruction it uses,
//     reproduces the golden digests including the full 50,000-round chain
//     (see the model checks in tests/selftest.cpp -- they run on every arch);
//   * this file compiles clean with a real x86 toolchain.
//
// What has NOT been verified is that the model matches silicon in every corner.
// risk.cpp therefore refuses to select this back end unless verify_backend()
// reproduces the reference digests on the actual machine. A wrong back end
// costs throughput, never a wrong answer.
#include "chain_backend.hpp"

#if defined(__x86_64__) || defined(__i386__)

#include "chain_x86_core.hpp"
#include "chain_x86_ops.hpp"

namespace obsidio {
namespace chain {
namespace {

void chain1_impl(const char in[64], int rounds, char out[64]) {
  chain1_core<X86Ops>(in, rounds, out);
}

void chain2_impl(const char in_a[64], const char in_b[64], int rounds,
                 char out_a[64], char out_b[64]) {
  chain2_core<X86Ops>(in_a, in_b, rounds, out_a, out_b);
}

const Backend kX86Backend = {"x86-sha-ni (x2 interleaved)", chain1_impl,
                             chain2_impl};

bool cpu_has_shani() {
  __builtin_cpu_init();
  return __builtin_cpu_supports("sha") && __builtin_cpu_supports("ssse3") &&
         __builtin_cpu_supports("sse4.1");
}

}  // namespace

const Backend* x86_shani_backend() {
  return cpu_has_shani() ? &kX86Backend : nullptr;
}

}  // namespace chain
}  // namespace obsidio

#else  // !x86

namespace obsidio {
namespace chain {
const Backend* x86_shani_backend() { return nullptr; }
}  // namespace chain
}  // namespace obsidio

#endif
