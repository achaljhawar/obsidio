// The /risk hot path: ~98% of the CPU this service burns, and the file most
// likely to silently produce a wrong digest, which scores zero.
#pragma once

#include <cstddef>
#include <string>

namespace obsidio {

constexpr int kRiskIterations{50000};

// SHA-256 the seed, hex-encode, feed the hex back in, `iterations` times.
// The parameter exists so the self-test can check short chains.
std::string risk_hash(const std::string& seed, int iterations = kRiskIterations);

// Two independent chains in lockstep on one thread. A single chain is bound by
// SHA instruction latency, so the second rides the pipeline gaps nearly free.
void risk_hash_x2(const std::string& seed_a, const std::string& seed_b,
                  std::string& out_a, std::string& out_b,
                  int iterations = kRiskIterations);

// Three chains in lockstep, same contract.
void risk_hash_x3(const std::string& seed_a, const std::string& seed_b,
                  const std::string& seed_c, std::string& out_a,
                  std::string& out_b, std::string& out_c,
                  int iterations = kRiskIterations);

// Four chains in lockstep, same contract.
void risk_hash_x4(const std::string& seed_a, const std::string& seed_b,
                  const std::string& seed_c, const std::string& seed_d,
                  std::string& out_a, std::string& out_b, std::string& out_c,
                  std::string& out_d, int iterations = kRiskIterations);

// Eight chains in lockstep. On x86 this runs a phase-split kernel pipelined
// across lane groups; falls back to two x4 groups when there is no chain8.
void risk_hash_x8(const std::string seeds[8], std::string out[8],
                  int iterations = kRiskIterations);

// How many chains the selected back end genuinely advances in lockstep, after
// nulling any lane that failed verification. Not "the widest xN that exists":
// composing past this width leaves an odd chain running at the 1-lane rate.
int risk_lane_width();

// Selects and self-verifies the back end. Lazy, but call it at startup so any
// fallback shows up before the first request.
void init_risk_backend();

// Name of the selected back end, for the startup banner.
const char* risk_backend_name();

// True when an accelerated back end exists but its core failed verification,
// forcing the slow fallback. The self-test fails the build on this.
bool risk_backend_rejected();

// True when the core verified but a wider lane did not and was disabled.
bool risk_backend_partial();

// The RISK_BACKEND value this process was started with, or nullptr.
const char* risk_backend_forced();

}  // namespace obsidio
