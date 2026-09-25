# Engine & hosting plan (no UI)

Planned 2026-09-24 (Fable). Adapted the same day: no CM4 access, so M0 runs on the CM5 with
estimated CM4 ranges; real CM4 numbers come from test-build testers.

## Facts that drive it (dsp56300 paths relative to `build-mac/_deps/dsp56300-src/source`)

- JIT executable memory: one asmjit `JitRuntime` per engine (`dsp56kEmu/jit.cpp:90`), pools that
  double up to 32 MB (`asmjit/src/asmjit/core/jitallocator.cpp:483-557`) — mmaps are logarithmic in
  code size. `MmuArray` (JIT per-PC function table + cache) does munmap + mmap(MAP_FIXED) + mlock on
  the first compile into each fresh P region (`dsp56kBase/mmuarray.h:186-213`). Compile-time only,
  but dozens per engine during warm-up → in-process JIT would take MoveOriginal's `mmap_lock`.
- DSP data RAM is mapped fully at construction (`dsp56kEmu/memorybuffer.cpp:97-131`), shm-backed.
- Compiled code cannot be shared or snapshotted across engines.
- Non-JIT time: every HI08 word goes through a locking `RingBuffer` + `setDelayCycles(0)`
  (`hdi08.cpp:152-162, 300-331`); peripherals tick every 32 instructions (`dsp.h:82`) though the
  harness uses no ESSI/DMA/timers; `getenv` per block (`DspEngine.cpp:169`).
- A machine change needs no DSP reset: `HostModel::setMachine` + forceInit; new code JITs lazily.
- Osirus precedent (`schwung-virus/src/dsp/virus_plugin.cpp`): fork per instance, free-running
  child, ~17 ms ring. Flaws not to copy: never demotes from FIFO 70; `fork()` of MoveOriginal.

## 1. Hosting model

One **spawned helper process per module instance** (`mnm-engine`, `posix_spawn`, not `fork`),
running all N engines of that instance on up to 3 worker threads pinned to cores 0-2. The plugin
`.so` in MoveOriginal only copies rings, forwards commands and wakes the child.

- Not in-process: crash isolation (patched, less-travelled aarch64 JIT) and mmap_lock at compile.
- Not fork: page-table copy + COW faults in MoveOriginal.
- Not one shared server: no host lifetime support, single point of failure.
- Nothing renders on the callback (CM4 estimate 20-30% of a core per engine).

Boot thread (demoted to SCHED_OTHER, cores 0-2 first) spawns the child with a memfd. Child:
dispatcher + K = min(N,3) workers, FIFO 10 with SCHED_OTHER fallback, never core 3. Parent keeps
the authoritative param table; watchdog respawns on frozen heartbeat and replays it. Reap on a
detached thread. `capabilities.forks_processes: true`.

Shared memory per instance: versioned header (host_block futex, heartbeat, boot phase, error,
per-engine stats), SPSC command FIFO (16-byte records: param/LFO/level/machine/routing/BPM/note/
tune/reset/deps), per-engine input/output slots of `D+1` × 128 stereo int32. Callback per block:
write FX input, read output slot k−D, bump host_block, futex wake. **Deadline-driven**: the child
renders exactly 128 frames (8 engine blocks) per wake; latency D×128 frames (+16 for FX).

N engines: one `MonoVoice` each (no multiplexing). Per-block dependency DAG (default none = fully
parallel); serial routings (NEIBOR/BUS) become chains; mixing between engines happens in the child.
One output ring per engine; parent sums, and `render_split` stays possible.

## 2. Performance program (BE = bit-exact vs unmodified upstream)

| # | Item | Gain | BE? |
|---|---|---|---|
| 1 | Off-callback hosting + 3-core parallelism | structural | BE |
| 2 | Idle parking: skip DSP after output (and FX input) exactly 0 for > longest tail (~2 s), keep `HostModel::nextBlock()` running; wake on note/param/input | ~100% of an idle engine | **not BE at wake** |
| 3 | Lean harness: poke the 52 words into Y:$500 directly, read output by peek, one sentinel word over HI08 | ~8-10% | BE |
| 4 | Remove per-block getenv | ~0.5% | BE |
| 5 | Compiler flags: -mcpu=cortex-a72, LTO, -fno-plt, no fast-math | 1-3% | BE (verify) |
| 6 | Peripheral exec: skip ESSI clock when disabled / larger step size | ~2-3% | verify |
| 7 | Pre-sized JIT pools | smoother switches | BE |
| 8 | Pre-warm all 22 machines per engine at boot | no first-switch spikes | BE |
| 9 | JitConfig experiments (multi-wrap modulo, maxDoIterations) | 0-10% | A/B decides |
| 10 | checkModeChange micro-opts | ≤2.5% | only if short |

Every change ships a bench delta and an identical `mnm-golden` result (item 2 ships a divergence
report instead). Upstream code is never edited in the submodule: our harness is a copy in `src/`,
dsp56300 changes are a second patch applied after upstream's.

## 3. Engine lifetime

OS file via the module.json `assets` block; `fw::loadFirmware` checks section checksums. The child
also looks in the sibling module's dir so the user uploads once. Boot in the child: parse firmware,
create engines in parallel, optional pre-warm, publish ready; the parent plays silence and queues
state until then. State = parent-held table per engine, restored by replaying commands + forceInit.

## 4. Milestones (UI excluded)

- **M0** — bench: threaded `--engines N`, RSS, init split, mmap count; CM5 numbers + CM4 estimates;
  check FIFO 10 for a spawned non-root binary and memfd.
- **M1** — BE items 3-7, 9 with the golden gate.
- **M2** — `mnm-engine` + parent library + Monomodule One (1 engine, bare test UI). Gate: 0
  underruns over 10 min, child kill → audio back with state, CPU page attribution, latency by loopback.
- **M3** — N engines + scheduler + parking + Monomodule FX. Gate: 6-engine instance within budget
  with 2 active / 4 parked.
- **M4** — state round-trip, assets, pre-warm policy, packaging of both modules. Then stop: UI next.

## Josh's decisions (ranked)

1. Pipeline depth: D=1 (2.9 ms) vs D=2 (5.8 ms, absorbs one bad block).
2. Idle parking is not bit-exact at wake: accept, or restrict to machines that converge exactly.
3. Worker class: FIFO 10 on cores 0-2 vs SCHED_OTHER (more jitter → larger D).
