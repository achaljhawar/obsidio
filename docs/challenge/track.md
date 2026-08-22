# Obsidio: Resilient Backend

> **Source document:** this preserves the original track overview. See the
> [detailed specification](spec.md) for the contract and the
> [project README](../../README.md) for the current implementation.

> *Obsidio is the siege. Your task is to build a backend that stays standing
> under pressure. This is the purest engineering track; there is no essay to
> write your way through and no design to hide behind. Your system either
> holds, or it does not.*

Most tracks are judged on what you *say* about your work. This one is judged on what your work *does*.

You'll build a backend to a given spec, and on the day it will face real load. Not a description of load, actual traffic, measured. The grade isn't a matter of opinion: it's whether your system stays responsive when it's pushed, and how gracefully it degrades when it can't.

**Your job is to build something that survives contact with reality.**

## Where to start

- Understand your bottlenecks before you're told where they are. Profile early.
- Think about what happens at the edges: timeouts, retries, resource exhaustion.
- Measure your own system under stress before we do. Bring numbers.
- Resourcefulness is encouraged: load tools, profilers, and libraries are *engineering*, not cheating. The line is that the thinking has to be yours.

## What to submit

- A working backend deployed to the provided spec.
- A short **resilience write-up**: where your bottlenecks were, what you did about them, and your own measured results under stress.
- A video pitch on your architecture and the trade-offs you chose.

## More information

Read the [detailed specification](spec.md) and
[directions](directions.md) for the full rules and resources.
