// Risk-only saturation probe: the ~1%-noise A/B instrument.
//
// The full grading ramp measured a 40% spread between identical runs on the
// Ryzen laptop (obsidio-findings.md section 10), which makes it useless for
// deciding a 3% question. A short constant-VU risk-only run repeats to ~1%
// because it skips the two things that carried the noise: the VU ramp and the
// connection churn at 200 VUs.
//
// Use this to compare kernels or back ends. Use the real grading script only
// to confirm thresholds pass and to produce a headline number.
import http from 'k6/http';
import { check } from 'k6';

const BASE = __ENV.TARGET || 'http://localhost:8080';

export const options = {
  scenarios: {
    probe: {
      executor: 'constant-vus',
      vus: Number(__ENV.VUS || 16),
      duration: __ENV.DURATION || '40s',
    },
  },
  // No thresholds: this probe answers "how fast", not "does it qualify".
  thresholds: {},
};

export default function () {
  const res = http.get(`${BASE}/risk?seed=${Math.random()}`, {
    tags: { name: 'risk' },
  });
  check(res, { ok: (r) => r.status === 200 });
}

// One machine-readable line so the A/B harness never has to scrape the pretty
// summary, whose layout changes between k6 versions.
export function handleSummary(data) {
  const m = data.metrics;
  const reqs = m.http_reqs ? m.http_reqs.values.count : 0;
  const rate = m.http_reqs ? m.http_reqs.values.rate : 0;
  const failed = m.http_req_failed ? m.http_req_failed.values.rate : 0;
  const dur = m.http_req_duration ? m.http_req_duration.values : {};
  const line =
    `PROBE_RESULT reqs=${reqs} rate=${rate.toFixed(6)} ` +
    `fail_rate=${(failed * 100).toFixed(4)} ` +
    `p95=${(dur['p(95)'] || 0).toFixed(3)} med=${(dur.med || 0).toFixed(3)}\n`;
  return { stdout: line };
}
