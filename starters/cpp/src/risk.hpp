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

// Three chains in lockstep, same contract as risk_hash_x2. ~2.2x a single
// chain against x2's ~1.96x. The pool prefers this and steps down to x2 then
// x1 as the queue drains.
void risk_hash_x3(const std::string& seed_a, const std::string& seed_b,
                  const std::string& seed_c, std::string& out_a,
                  std::string& out_b, std::string& out_c,
                  int iterations = kRiskIterations);

// Four chains in lockstep, same contract again. The pool prefers this and
// steps down through x3, x2, x1 as the queue drains.
void risk_hash_x4(const std::string& seed_a, const std::string& seed_b,
                  const std::string& seed_c, const std::string& seed_d,
                  std::string& out_a, std::string& out_b, std::string& out_c,
                  std::string& out_d, int iterations = kRiskIterations);

// How many chains the selected back end advances in genuine lockstep, after
// accounting for any lane that failed verification. 1 when no accelerated back
// end qualified, since the fallback gains nothing from grouping.
//
// This is what the pool batches to. It is NOT "the widest risk_hash_xN that
// exists": every xN is callable and correct, but on a back end whose lane
// width is 2, x3 composes to chain2 + chain1 and the odd chain runs at the
// 1-lane rate -- measurably worse than not grouping it at all.
int risk_lane_width();

// Selects and self-verifies the hash back end. Called lazily by risk_hash(),
// but call it explicitly at startup so the cost and any fallback show up before
// the first request rather than during one.
void init_risk_backend();

// Human-readable name of the selected back end, for the startup banner.
const char* risk_backend_name();

// True when this CPU offers an accelerated back end but its core (chain1 or
// chain2) failed to reproduce the reference digests, so the slow fallback is
// in use. Always false when the architecture simply has no back end. The
// self-test fails the build on this.
bool risk_backend_rejected();

// True when the accelerated core verified but one of the wider lanes (chain3 /
// chain4) did not, so that lane was disabled and the pool degrades to fewer
// lanes. Correct answers either way; the self-test reports it loudly.
bool risk_backend_partial();

// The RISK_BACKEND value this process was started with, or nullptr. Lets the
// self-test distinguish "forced back end absent on this CPU, skip" from a real
// verification failure.
const char* risk_backend_forced();

}  // namespace obsidio
