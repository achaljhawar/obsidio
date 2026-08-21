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

}  // namespace obsidio
