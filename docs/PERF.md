# Performance log

Measured with `mnm-bench` (steady state; synths hold a note, FX machines process noise). Cost per
128-frame Move block against the 2902 us deadline, one engine.

## 2026-09-24 — baseline, unmodified upstream engine

| where | mean | p99 | notes |
|---|---|---|---|
| Mac (Apple Silicon) | 2.5-3.9% (73-113 us) | 2.7-7.1% | |
| Move CM5 (A76 2.4 GHz), Move running, pinned core 3 | 7.3-11.1% (211-321 us) | 11.7-15.9% | max outlier 1453 us (VO-6) |
| Move CM4 (ship target) | not yet measured | | |

Cost is flat across all 22 machines: 5.5k-9.9k DSP instructions per 16 frames, and a silent GND
track costs as much as FM. Engine init is 180-230 ms on CM5.

Mac profile (SID): ~78% in JIT-generated code; the rest is harness and peripheral overhead —
host-port ring buffers ~7.5%, the run loop ~3.5%, peripheral ticking ~3%, JIT mode checks ~2.5%,
a per-block `getenv` ~0.5%.

## Bit-exactness gate

`mnm-golden <os.syx>` renders a fixed script on all 22 machines (notes, parameter sweeps on every
page, an LFO, retriggers, release tails; FX machines take noise that stops halfway) and prints a
hash per machine. `tests/golden-baseline.txt` holds the hashes of the unmodified upstream engine.
Every optimisation must reproduce it exactly. Mac and Move builds give identical hashes, so the gate
can run on the Mac.
