# Obsidio Directions & Resources

<aside>
⚔️

This page is the **how-to**. The track overview told you what Obsidio is; this page covers the siege rules, the box you deploy into, the exact API you build, how it is graded, and how points are scored. Read it fully before you write code. The constraints here are the meat of the whole game.

</aside>

## The one-sentence version

Build a small analytics API, containerise it to our spec, and keep it fast and correct while we flood it with heavy traffic on a box that's identical for every team.

## The load, in plain terms

We measure your backend under heavy traffic to see how well it holds up. This is the same load testing real engineering teams run against their own systems before shipping, using a standard tool called k6. Two ground rules keep it fair: every team runs in the same resource-capped box, and we publish the exact load and thresholds in advance. There's no hidden test; you optimise against the same script we grade with.

## The environment

Your backend runs in Docker, capped identically for all teams.

- **You submit a `Dockerfile`.** It builds your app in any language and framework. The inside of the container is yours.
- **Listen on port `8080`.** We build your image and run it ourselves at grading time.
- **Build reproducibly.** Pin your versions; don't rely on `latest` tags. If it builds differently on our machine, that's on the submission.
- **The box is capped at 2 CPUs and 2 GB RAM, total, per team.** If you add services (see the persistence bonus), that budget is shared across all of them.

> ⚠️ **The core-count gotcha.** Your container is CPU-throttled but can still *see* all of the host's cores. Some runtimes size their worker or thread pools from the visible core count (Go's `GOMAXPROCS`, the JVM, Node's cluster module, Uvicorn and Gunicorn workers). If yours does, it may spawn workers for cores it can't use and crater your performance. Set your worker count explicitly to match the 2-core cap.
> 

New to Docker? Every starter in the code bundle already builds and runs, so you clear that hurdle for free. See the [Docker guide] for a walkthrough.

## The task: a price analytics API

You're building a small service of the kind a trading or analytics firm runs internally. It answers three kinds of request, cheap to expensive, plus a health check. The mix of light and heavy work under load is the whole problem: a naive build lets the expensive requests clog the pipe so even the cheap ones crawl. A good build keeps the fast path fast no matter what the slow path is doing.

The work each endpoint does is specified exactly, so every team does identical work. You don't get to make the heavy endpoint cheaper; you only get to serve the same work more efficiently. Build all four endpoints exactly as below; every response is JSON.

| Endpoint | Weight | Does | Returns (200) | Errors |
| --- | --- | --- | --- | --- |
| `GET /health` | not scored | Liveness check | `{"status":"ok"}` | — |
| `GET /price?symbol=SYM` | 1 (cheap) | In-memory price lookup, near instant | `{"symbol":"AAPL","price":187.42}` | `404 {"error":"unknown symbol"}` |
| `GET /stats?symbol=SYM` | 3 (medium) | Compute mean, min, max, stddev over the symbol's 500-point series, every request | `{"symbol":"AAPL","mean":187.4,"min":183.6,"max":191.2,"stddev":2.1}` | `404 {"error":"unknown symbol"}` |
| `GET /risk?seed=VALUE` | 10 (heavy) | Apply SHA-256 to `seed`, hex-encode, feed back in, repeat 50,000 times | `{"seed":"0.48","risk_hash":"<final digest>"}` | — |

The valid symbols and 500-point series are in the starter code. Because `seed` differs every request, `/risk` can't be cached or precomputed; because it's deterministic, we can verify your digest (a fake or constant value fails and scores zero).

## How grading works

Per team, we `docker build` and `docker run` your container capped at 2 CPU / 2 GB, confirm `/health` returns 200, run our k6 script against it, and record your score.

The script ramps a growing crowd of virtual users hitting a fixed mix: **60% `/price`, 30% `/stats`, 10% `/risk`**, climbing to a sustained peak, then winding down. Same schedule for everyone.

To qualify, your build must clear these bars *(placeholders, finalised on the grading hardware before the event)*:

| Metric | Bar |
| --- | --- |
| `/price` p95 latency | under 200 ms |
| `/stats` p95 latency | under 500 ms |
| `/risk` p95 latency | under 1500 ms |
| Error rate | under 1% |

> ⚠️ Your `/price` p95 is the number that matters most. A price lookup does almost no work, so if it's slow under load, it's stuck in line behind heavy requests, not slow to compute. Keeping it low is the core skill. Using both cores is a start but usually isn't enough on its own.
> 

## What a strong submission looks like

Among builds that qualify, your score is the weighted count of requests served correctly *and* within the latency bars:

```
score = (1 x /price) + (3 x /stats) + (10 x /risk)
```

Late, errored, or timed-out requests don't count, which is why graceful behaviour wins: a build that sheds overflow fast and keeps its good requests quick out-scores one that accepts everything and lets it rot in a slow queue. There's no ceiling, so the best teams compete on how much they wring from the fixed box. We can tell the difference between a system that held because you engineered it to and one that held by luck; your write-up and pitch are where you show it was deliberate.

**Optional bonus: persistence under restart.** Entirely optional, and a real tradeoff rather than free points. Add a `POST /price` that records an update, back it with storage that survives a container restart, and you earn a bonus *if your data survives a mid-run restart AND you still clear every latency bar.* The catch: a database eats into your shared 2 CPU / 2 GB budget and a naive integration can drop you below the qualifying bar, costing more than the bonus is worth. Doing it cheaply without wrecking your resilience is the achievement. If you attempt it, submit a `docker-compose.yml` (shape in the code bundle).

## Where to start

- **Containerise first, not last.** Get the starter building and running under the caps on day one. The caps change how your app behaves.
- **Profile before you optimise.** Find your real bottleneck with evidence. Run the k6 script against your own container and read the numbers.
- **Watch the fast path.** If `/price` p95 climbs under load, ask why a request that does no work is slow. The answer points at your architecture.
- **Think about the edges.** Timeouts, overflow, more requests than you can serve. Failing fast beats hanging.

## What to submit

- A working backend as a `Dockerfile` (plus `docker-compose.yml` only if you attempt the bonus) that builds and runs to spec.
- A short **resilience write-up:** where your bottlenecks were, what you did, and your own measured k6 results. Numbers, not adjectives.
- A video pitch on your architecture and the trade-offs you chose.

## Resources

This is the GitHub repo for your skeleton code:

https://github.com/solpercival/Obsidio

## A note on judging

> ⚖️ The on-theme prize rewards resilience by design. We can tell a deliberately engineered system from a lucky one. Your write-up and pitch are where you prove the choice was yours.