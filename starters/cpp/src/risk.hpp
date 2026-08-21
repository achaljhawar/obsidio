// The /risk hot path, isolated on purpose.
//
// This is ~98% of the CPU this service burns, so it is the single file worth
// optimising. It is also the file most likely to silently produce a WRONG
// digest, which scores zero. Every change here must keep tests/selftest.cpp
// green.
#pragma once

#include <cstddef>
#include <string>

namespace obsidio {

constexpr int kRiskIterations = 50000;

// Apply SHA-256 to `seed`, lowercase hex-encode, feed the hex string back in,
// repeat `iterations` times. Returns the final 64-char hex digest.
//
// `iterations` is a parameter only so the self-test can check short chains.
// The endpoint always uses kRiskIterations.
std::string risk_hash(const std::string& seed, int iterations = kRiskIterations);

// Two independent chains advanced in lockstep on one thread. Identical results
// to calling risk_hash twice, ~1.45x the throughput: a single chain is bound by
// the latency of the SHA instruction dependency, not by issue width, so the
// second chain runs almost for free in the gaps. The risk pool pairs up queued
// requests to use this; it falls back to two sequential chains when only one
// request is available or no accelerated back end qualified.
void risk_hash_x2(const std::string& seed_a, const std::string& seed_b,
                  std::string& out_a, std::string& out_b,
                  int iterations = kRiskIterations);

// Selects and self-verifies the hash back end. Called lazily by risk_hash(),
// but call it explicitly at startup so the cost and any fallback show up before
// the first request rather than during one.
void init_risk_backend();

// Human-readable name of the selected back end, for the startup banner.
const char* risk_backend_name();

// True when this CPU offers an accelerated back end but it failed to reproduce
// the reference digests, so the slow fallback is in use. Always false when the
// architecture simply has no back end. The self-test fails the build on this.
bool risk_backend_rejected();

}  // namespace obsidio
