// Mixed-workload probe: same 60/30/10 weights as the grading script, but at
// constant VUs and short duration so it inherits the risk probe's low noise.
//
// This exists because the risk-only probe cannot answer the IO_THREADS
// question. /risk never runs on an IO thread -- it is handed to the worker
// pool -- so a risk-only load leaves the epoll loops nearly idle and any
// IO-thread configuration measures the same. Only a mix that actually keeps
// /price and /stats flowing can tell 1 IO thread from 2.
//
// It reports the same weighted work_score the grader uses, so the number is
// comparable in kind (not in magnitude -- this is a much shorter, flatter run).
import http from 'k6/http';
import { check } from 'k6';
import { Counter } from 'k6/metrics';

const BASE = __ENV.TARGET || 'http://localhost:8080';
const SYMBOLS = ['AAPL', 'GOOG', 'MSFT', 'AMZN', 'NVDA', 'META', 'TSLA', 'JPM'];
const pick = (a) => a[Math.floor(Math.random() * a.length)];
const workScore = new Counter('work_score');

export const options = {
  scenarios: {
    probe: {
      executor: 'constant-vus',
      vus: Number(__ENV.VUS || 32),
      duration: __ENV.DURATION || '40s',
    },
  },
  thresholds: {},
};

export default function () {
  const r = Math.random();
  let res;
  let weight;
  let tier;
  if (r < 0.6) {
    tier = 'price';
    weight = 1;
    res = http.get(`${BASE}/price?symbol=${pick(SYMBOLS)}`, {
      tags: { tier, name: 'price' },
    });
  } else if (r < 0.9) {
    tier = 'stats';
    weight = 3;
    res = http.get(`${BASE}/stats?symbol=${pick(SYMBOLS)}`, {
      tags: { tier, name: 'stats' },
    });
  } else {
    tier = 'risk';
    weight = 10;
    res = http.get(`${BASE}/risk?seed=${Math.random()}`, {
      tags: { tier, name: 'risk' },
    });
  }
  if (check(res, { ok: (resp) => resp.status === 200 })) workScore.add(weight);
}

export function handleSummary(data) {
  const m = data.metrics;
  const score = m.work_score ? m.work_score.values.count : 0;
  const rate = m.http_reqs ? m.http_reqs.values.rate : 0;
  const failed = m.http_req_failed ? m.http_req_failed.values.rate : 0;
  const price = m['http_req_duration{tier:price}'];
  const risk = m['http_req_duration{tier:risk}'];
  const line =
    `PROBE_RESULT score=${score} rate=${rate.toFixed(6)} ` +
    `fail_rate=${(failed * 100).toFixed(4)} ` +
    `price_p95=${price ? price.values['p(95)'].toFixed(3) : 0} ` +
    `risk_p95=${risk ? risk.values['p(95)'].toFixed(3) : 0}\n`;
  return { stdout: line };
}
