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

## Memory per engine (CM5, PSS — VmRSS overstates it ~4x because dsp56300 maps one page many times)

| change | PSS per engine |
|---|---|
| baseline | 183 MB |
| interpreter opcode cache only built when the interpreter is enabled (patch 0002) | 112 MB |
| ESSI audio ring buffers 32768 -> 256 frames (patch 0002; the harness streams no audio through ESSI) | 32 MB |

The rest: DSP RAM (~19 MB, shm, mlocked) and the JIT's per-mode tables and code.

## Several engines at once (CM5, SID on every engine, workers pinned to cores 0-2)

| engines | makespan mean | p99 | notes |
|---|---|---|---|
| 1 | 9-12% | 17-28% | SCHED_OTHER |
| 3 | 14.5% | 46% | SCHED_OTHER |
| 3 | 19.8% | 36% | SCHED_FIFO 10 (works without root) |
| 6 | 24-33% | 55-58% | 2 engines per core |

Tails grow with the engine count; the child must be allowed to run a block or two ahead.

## Machine switches (CM5, one engine on core 2): worst block after switching to each machine

Without pre-warm the first switch to a machine JIT-compiles its code in the audio path: 1.6-30 ms
(SWAVE SAW 25 ms, REVERB 23 ms, VO-6 30 ms) — an audible dropout, ~2-3x that on a CM4. The second
switch costs a normal block. `MonoVoice::prewarm()` renders every machine once (297 ms per engine on
CM5), then `DspEngine::resetKeepCode()` zeroes internal X/Y RAM and re-runs the kernel init without
touching P, so compiled code survives and the voice is exactly a fresh one (`MNM_PREWARM=1
mnm-golden` = baseline). After it every switch is a normal block (0.3-0.5 ms), except the DSP's own
work: REVERB ~2.5 ms once when engaged, VO-6 ~0.95 ms per block throughout.

## Inside Move (2026-09-25, CM5, stock Schwung 1.4.0, Monomodule One on track 1, notes as external MIDI)

`tools/devicetest/test_monomodule.py`: every machine plays, 0 underruns at latency 1 and 2 over 20 s
of notes every 150 ms with machine switches, engine kill -> back with state, Monomodule FX in fx1:
every FX machine processes, 0 underruns.

Engine time per 128-frame block inside Move: 400-670 us (VO-6 and REVERB peaks 2.2-2.5 ms) — about
1.5-2x `mnm-bench`. The CPUs run the `ondemand` governor (1.5-2.4 GHz on this CM5): the bench keeps
a core busy and boosts it, the real engine works in bursts and mostly runs near 1.5-1.6 GHz. **The
in-Move numbers are the real ones**; bench numbers are for comparing changes. A CM4's governor
range decides its real cost — another reason the test build must report load from real devices.

## Idle parking (not bit-exact on wake; Josh approved 2026-09-25)

An engine sleeps once no note is held and its output (and, for FX, input) has been silent at the
host's 16 bits for 2 s; the DSP is skipped, the host model (frame counter, LFOs, slew) keeps
ticking. Any command or non-silent input wakes it before the block renders. After a note every
synth machine is exactly silent ~2 s after note-off; THRU/CHORUS/PHASER/FLANGER within 0.2 s of the
input stopping; REVERB, DYNAMIX and RINGMOD settle into a 1-43 LSB residue (-106..-138 dBFS) that
is exactly zero at 16 bits (`mnm-bench --silence`).

Measured inside the plugin path on the CM5 (`mnm-hosttest one park`): engine process 0.4% of a
core asleep vs 15.2% playing (FM+ PAR, real clock under ondemand).

### Inside Move, the host idles silent slots first

Schwung pauses a synth slot after ~1 s of 16-bit silence (`DSP_IDLE_THRESHOLD`, then one probe render
per ~0.5 s), and the whole slot — FX included — once synth and FX are both silent. Measured in
Move: a silent Monomodule One and the FX behind it cost 0.0% CPU (engine process /proc stat), so our
2 s park never engages for a lone slot. It matters where the host cannot help: several engines in
one instance (one sounding, others idle — the DR32-style plan) and an FX whose slot keeps running.

## Bit-exactness gate

`mnm-golden <os.syx>` renders a fixed script on all 22 machines (notes, parameter sweeps on every
page, an LFO, retriggers, release tails; FX machines take noise that stops halfway) and prints a
hash per machine. `tests/golden-baseline.txt` holds the hashes of the unmodified upstream engine.
Every optimisation must reproduce it exactly. Mac and Move builds give identical hashes, so the gate
can run on the Mac.
