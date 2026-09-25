# Monomodule UI proposal (for Josh)

## 1. The concept

Ship **three modules that share one engine**: **Monomodule One** (a synth: one Monomachine track, played from the keys), **Monomodule Kit** (a drum-rack-style instrument: up to 6 Monomachine tracks played from the 32 pads), and **Monomodule FX** (the 7 FX machines as an audio effect). Kit is where the DR32 approach is used deliberately — but with one intentional difference: in DR32 an engine is cheap, so *each pad owns one*. A Monomachine engine is expensive (est. 30-45% of a CM4 core while sounding, ~0 idle), so in Kit the engines are a **pool of 6 "tracks"** — exactly the hardware's idiom — and a **pad is (track, note)**. Hitting a pad plays that note on that track; hitting it also focuses the track, so the knobs show that track's pages, DR32-style. Tracks are monophonic, as on the hardware, so the voice count and the CPU cap are the same number, and the UI makes that number visible. No polyphony-by-cloning in v1.

## 2. The surface

**Monomodule One** (keys, `chromatic`), jog order:

```
Presets  <browser>  presets = kit tracks from your .syx dumps + My Presets (host)
Track    MACHN LEVEL SNDA  SNDB  · LOAD(ro) LTNCY               <- machine picker is an enum (22 names)
SYN      the focused machine's 8 knobs, hardware labels          <- one page per machine, only the current one shows
AMP      ATK HOLD DEC REL | DIST VOL PAN PORT                    <- envelope picture on the first row
FILT     BASE WDTH HPQ LPQ | ATK DEC BOFS WOFS
EFX      EQF EQG SRR DTIM | DSND DFB DBAS DWID
LFO1..3  PAGE DEST TRIG WAVE | MULT SPD INTL DPTH
My Presets · Module                                              <- host appends these
```

- Labels are the hardware's own 4-character names (they fit the 5-char cell exactly; the held-knob header spells the full name). Bipolar knobs (TUNE, PAN, DIST, BOFS, EQG...) read -64..+63 with a sign, as the LCD does. Blank hardware slots ("-") are real gaps, not dead knobs.
- List-type SYN knobs become real pickers: SID WAVE shows TRI/SAW/PULS/MIX/NOIS, FM ratios show 1/64...4, DPRO waves S01-S32/D01-D64, VO-6 consonants. Turning them flashes the option list, clicking opens it.
- Switching machine swaps the SYN page in place (and restores that machine's last knob values, as upstream One does).
- LFO DEST shows the destination NAME for the chosen PAGE (1OCT, PAR3, DEC, CC 1...) — a small custom cell, not a canvas.

**Monomodule Kit** (pads, `drums`), jog order:

```
Kits     <browser>  kits from your dumps + My Presets
Pad      PAD  TRACK NOTE  ·  ·  ·  ·  ·                          <- 32 pads; PAD follows the pad you hit
Track    TRK  MACHN LEVEL SNDA SNDB FXIN TRKS LOAD(ro)           <- 6 tracks; TRK follows the hit pad's track
SYN / AMP / FILT / EFX / LFO1-3                                  <- the focused TRACK's pages, as in One
My Presets · Module
```

- **Pad page**: each pad has a TRACK (Off, 1-6) and a NOTE. Default map: pads 1-8 = track 1, 9-16 = track 2, 17-24 = track 3, 25-32 = track 4, each block an 8-note scale run — so a fresh Kit is immediately a 4-sound instrument with a bit of melody per sound. Move's sequencer records pad notes as usual, so a clip plays the right track at the right pitch.
- **Track page**: TRKS (how many tracks exist, 1-6, default 4 — tracks above it are off and their pads are silent), LOAD (read-only %, the engine process's real cost, live), FXIN for a track running an FX machine (NEIBOR = the previous track, or BUS AB/CD/EF; inert on synth machines). Copy/Delete-hold works on tracks (host feature).
- Focus: a pad hit moves PAD and TRK together (transport stopped: any note; running: only a real finger press, as DR32 does). Changing machine on a track swaps its SYN page.

## 3. The FX module

**Monomodule FX** sits in an audio FX slot and processes whatever is in front of it (a Schwung synth, Move's own track). Pages: `Machine` (THRU/REVERB/CHORUS/DYNAMIX/RINGMOD/PHASER/FLANGER, LEVEL, LTNCY) · the machine's SYN page (DEC DAMP GATE MIX HP LP - INP etc.) · AMP (held open, still gives DIST/VOL/PAN) · FILT · EFX · LFO1-3. No input-source knob: its input is the slot's audio. "One → FX(REVERB)" is the Move-native way to build the hardware's track-feeds-NEIBOR chain across modules; inside Kit, NEIBOR/BUS does it within one instance.

## 4. Presets and kits

- **Host My Presets** works for free on all three: One's state is one track (= a Monomachine "preset"), Kit's state is the whole kit plus the pad map and routing. Save/Save As/Delete live there.
- **Import**: a `dumps/` folder (uploaded through the web manager) holds the user's Monomachine `.syx` dumps. One's Presets browser lists every kit track from them (filter: this machine / all machines, as upstream's library does); Kit's Kits browser lists whole kits (loads 6 tracks, levels, routing; keeps your pad map). Duplicates across dumps merge (upstream's catalogue).
- **Factory content**: upstream ships none, and none is bundled here. v1 ships an **Init** preset per machine (the machine's defaults) and a handful of in-house demo kits made on the device. Export back to `.syx`: later (the encoder exists).

## 5. CPU honesty

- **Monophonic tracks = the voice cap.** 6 tracks is 6 voices, full stop. No "Poly" in v1: two engines per note would be 60-90% of a CM4 core for one sound.
- **TRKS is the budget knob.** Default 4 in Kit; the LOAD cell beside it shows what the engines actually cost right now, from the engine process's own timing. Beta testers on CM4s read this number; it is also what the load logging reports.
- **Idle is free.** A track that has not sounded for 2 s is parked (~0.4% vs ~15% on CM5); all 22 machines are pre-warmed per engine at boot so a machine switch never drops audio. While the engines boot the module says "Loading" instead of silently doing nothing.
- **Overload is visible, not silent.** Sustained underruns raise the module's error line on the chain card ("Engines overloaded: lower TRKS") rather than glitching quietly.

## 6. v1 scope vs later

**v1**: One, Kit, FX; SYN/AMP/FILT/EFX/LFO1-3 with hardware labels, pickers and bipolar readouts; pad map (track+note); host presets; `.syx` import browser (presets and kits); TRKS/LOAD; NEIBOR/BUS input for FX tracks in Kit; per-track buses and sends for Move-side FX (section 6 below).
**Later**: POLY mode (the hardware's own, tracks as voices — gated on real CM4 numbers); ASSIGN page (VEL/KEY/PB/MW to two targets each — needs engine-side support first, and it is what makes velocity do anything); OUT BUS AB/CD/EF mix-bus routing and INP (line-in) inside Kit; MULTI MAP key-zone play on a chromatic layout; `.syx` export; a pad-map overview canvas; kit trig/legato flags; patterns (Move's sequencer is the sequencer).

**Routing (Q6)**: Kit publishes its **6 tracks as splittable voices** (ids t1..t6, labels = the machine names), so on Schwung 1.3+ a track can go to its own module bus with its own inserts and sends — hats through the Schwung reverb, bass dry — and SNDA/SNDB on the Track page are per-track send levels. The engine child already renders one output ring per engine, so this costs nothing new. A bus is a *track*, never a pad; pads are notes.

## 7. Your decisions, ranked

1. **Three modules (One / Kit / FX) vs one synth module with a layout switch.** Recommend three. A synth's pad layout is served, so one module *could* flip between keys and pads, but consumers do not re-read it reliably and the two have different page walks; three ids with one engine binary is honest and cheap.
2. **What a Kit pad is.** Recommend (track, note) over a 6-engine pool. Alternatives: pad = its own engine (DR32-literal; 32 × 32 MB and 32 processes' worth of CPU — not possible), or hardware MULTI MAP zones on a chromatic layout (authentic, but zones on Move's in-key pad grid are hard to read and Move's sequencer gains nothing). Pads sharing tracks is the one that plays and sequences like a Move drum rack.
3. **Engine budget.** Recommend Kit default TRKS = 4, max 6, LOAD readout always on, no polyphony in v1. Alternative: always 6 (simpler, but the honest CM4 number is unknown until the beta reports it).
4. **How machine-specific SYN pages are served.** Recommend the DR32 way: one small level per machine, gated on the focused track's machine (22 gated levels, ~20 KB served). Fail-open shows every machine's page — cluttered, never wrong — and following a *pad hit* into the right page needs upstream #533, exactly as DR32 does. Alternative: re-serve the hierarchy on every machine/focus change via the `is_loading` pulse (works on 1.4.0 alone, but re-plans the grid under your hand on every pad hit).

---

## Appendix: implementation notes (file/field references)

- **Hierarchy must be served**: sound generators' `ui_hierarchy` comes only from `get_param("ui_hierarchy")` (`/Users/josh/schwung repos/schwung-current/docs/MODULES.md` ~1975); FX may declare it in module.json. Keep `chain_params` in module.json in sync (generated, like DR32's `tools/gen_engine_ui.mjs`) — it feeds modulation/patch metadata. Limits: module.json 64 KB, served hierarchy 128 KB (memory `schwung-dr32-multiengine.md`). 22 machine levels × 8 params is ~20 KB; the 3 LFO pages, AMP/FILT/EFX are shared.
- **Unique keys everywhere** (memory `hierarchy-duplicate-key-kills-per-voice-sends.md`): per-machine SYN keys need a prefix (`sid_wave`, `fmp_1frq`, ...) that the DSP aliases onto the focused track's 8 raw SYN slots; the sends' dB range is read from the C side and refused on any duplicate.
- **Template shape + `visible_if`** (memory `ui-hierarchy-sibling-shape-per-voice-params.md`): Kit's Track/SYN/AMP/FILT/EFX/LFO levels are child levels (`child_count: 6`, `child_key_template: "t{index}_{key}"`, `child_index_base: 1`, `child_index_param: "ui_track"`); the Pad level is `child_count: 32`, `child_index_param: "ui_pad"`, `child_note_base: 36`, `child_press_param: "ui_live_press"`, `pad_layout: "drums"`. Gates: `ui_machine` (focused track's machine, derived, read-only, on NO level) for SYN pages; `machine`/`t{n}_machine` is the user's enum cell. Host evaluates at most four distinct gate params (DR32 CLAUDE.md, Stereo page note). Listing `ui_pad`/`ui_track` as knob 1 suppresses the host's generated picker page (DR32 CLAUDE.md, `childPickerNeeded`).
- **Bipolar/enum display**: declare bipolar knobs `int min:-64 max:63` and offset ±64 on the wire; the grid signs the value (`docs/PARAM_PAGES.md` "Small ints are BIG NUMBERS", sign only with a negative side). Lists must be `type: "enum"` with `options` (memory `a-labelled-param-must-say-enum.md`); the module maps index ↔ raw with `spec::listIndex/listRawMid` (`libs/monomodule/src/plugin/one/SpecData.h`). Labels/defaults/bipolar masks: `libs/monomodule/src/core/host/Machines.h`; list names and display types: `libs/monomodule/src/plugin/one/SpecData.cpp`. Blank slots: `""` in `knobs` (memory `param-pages-empty-knob-slot.md`).
- **LFO DEST cell**: `viz: {kind: "custom:lfodest", extra_keys: ["lfo1_page"]}` served in the plugin's `chain_params` (must equal the host's fallback plus the viz — memory `schwung-custom-knob-widgets.md`; DR32 `tools/check_chain_params.mjs` is the proof pattern), `canvas.js` draws `kLfoDestNames[page][dest]` in the 32x15 box (caps-only font: names are already caps; check `/` and digits). LFO params are host-side (`libs/monomodule/src/core/host/Lfo.h`, `HostModel::setLfoParam`) and not yet exposed by `src/plugin/mnm_plugin.cpp` (param table there: `machine`, `level`, `depth`, `syn1..efx8`).
- **AMP viz**: `viz: {group:"amp", role: attack|hold|decay|release}` on the first four knobs (one row of four — memory `viz-graphic-must-fit-one-row-of-four.md`).
- **Machine swap → labels**: with gating no re-read is needed; on a 1.4.0 host without #533 the gate follows the machine knob (it is on the page) but not a pad hit (fail-open = union). Track/pad *names* in the header (`child_names`) need DR32's `is_loading` pulse to refresh (DR32 CLAUDE.md "PAD NAMES FOLLOW THE SAMPLE ONLY BECAUSE DR32 SERVES is_loading"; must always answer "0"/"1").
- **Multi-engine hosting already fits**: `src/common/mnm_shm.h` has `kMaxEngines = 8`, one output ring per engine (`out[kSlots][kMaxEngines]`), a per-command `engine` field; `docs/PLAN.md` §1 promises the NEIBOR/BUS dependency DAG in the child and "parent sums, render_split stays possible". Kit needs: per-engine command routing in `mnm_plugin.cpp` (today `cmd.engine = 0`), a pad→(track,note) table, `split_voices` = `[{"id":"t1","label":"<machine>"},…6]` + `move_plugin_render_split` accumulating per-engine rings (both paths must stay state-compatible — trivial here, the child renders regardless), `voice_send_params` = `["{id}_send_a","{id}_send_b"]` with `unit:"dB"` declared on the Track level.
- **CPU facts**: `docs/PERF.md` — CM5 ~7-11% mean per engine on the bench, 400-670 µs/block (≈15-23%) inside Move under `ondemand`, VO-6/REVERB peaks 2.2-2.5 ms; 6 engines 24-33% makespan on 3 cores; parked engine 0.4%; pre-warm 297 ms/engine (CM5). CM4 unmeasured (est. 2-3×). LOAD cell = `stats[k].lastUs` summed / 2902 µs, served `live: true`; overload → `get_error` (host reads it via `v2_synth_get_error`, `chain_host.c:198`).
- **Presets**: host store is `/data/UserData/schwung/presets/<module-id>/*.json` wrapped `{name,module,version,state}` (memory `schwung-module-preset-location.md`); Init presets ship there. `.syx` parsing: `libs/monomodule/src/core/library/MnmDump.h` (`Kit`, `KitTrack.params[72]`, `fxInput()`, `outBuses()`), dedup catalogue `Catalog.h` (`PresetItem`, `KitItem`). Browser = DR32's `kits`/`kit_list` pattern (items page → `list_param/count_param/name_param` level, incremental scan off the callback; note the host preset page auditions with no undo, DR32 CLAUDE.md).
- **Canvas**: none needed; an `as_page` canvas would only be for a later pad-map overview (memory `canvas-page-rules.md`).

### Critical Files for Implementation
- /Users/josh/schwung repos/schwung-monomodule/src/plugin/mnm_plugin.cpp
- /Users/josh/schwung repos/schwung-monomodule/libs/monomodule/src/core/host/Machines.h
- /Users/josh/schwung repos/schwung-monomodule/libs/monomodule/src/plugin/one/SpecData.cpp
- /Users/josh/schwung repos/schwung-dr32/tools/gen_engine_ui.mjs
- /Users/josh/schwung repos/schwung-current/docs/MODULES.md

