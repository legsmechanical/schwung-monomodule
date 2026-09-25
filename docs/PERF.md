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

## Optimisation log (CM5, Move running, bench pinned to core 2, 3-run averages)

| change | mean load | bit-exact |
|---|---|---|
| baseline (core 3 run) | 9.2% | — |
| lean harness: block written straight to DSP memory, one HI08 word each way | 8.3% (core 3) / 8.0% (core 2) | yes |
| `-mcpu=cortex-a72` + LTO | 7.8% | yes |
| `aguSupportMultipleWrapModulo=false` (Mac) | no change — dropped | yes |
| skip the kernel's NOP padding loops (4 sites; GND 8755→5468, GND SIN 9877→4766, THRU 8865→5578 instr/16f) | 7.3% (heaviest: SID / DDRW / DENS 9.3%) | yes |

The padding loops were found with a per-PC instruction histogram and `mnm-disasm <os> --idle-loops`,
which lists every DO whose body is only NOPs; each was checked by hand to be code, not data.

After the lean harness the Mac profile is ~87% JIT-generated code. The DSP hands control back to
the run loop ~190 times per 16 frames (~43 instructions per `exec()`), so the remaining non-JIT
cost is that loop plus peripheral ticking (~2%).

## Bit-exactness gate

`mnm-golden <os.syx>` renders a fixed script on all 22 machines (notes, parameter sweeps on every
page, an LFO, retriggers, release tails; FX machines take noise that stops halfway) and prints a
hash per machine. `tests/golden-baseline.txt` holds the hashes of the unmodified upstream engine.
Every optimisation must reproduce it exactly. Mac and Move builds give identical hashes, so the gate
can run on the Mac.
