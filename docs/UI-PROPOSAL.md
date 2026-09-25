# Monomodule UI proposal (rev 2)

Rev 2 (2026-09-25), after Josh: **no kits — an instrument and an effect only.** The DR32 reference is
to how one UI manages several engines and adjusts to the one currently selected; here an "engine" is
a Monomachine *machine*.

## 1. The concept

Two modules, one engine each:

- **Monomodule One** (sound generator, played from the keys): the 15 synth machines.
- **Monomodule FX** (audio FX): the 7 FX machines, processing the slot's audio.

In both, the **Machine** knob is the engine picker, and the pages follow it the way DR32's pages follow
a pad's engine: the SYN page becomes that machine's own eight controls with the hardware's labels,
and anything that does not apply to the current machine is not shown.

## 2. Monomodule One — the pages (jog order)

```
Presets  browser: .syx dump sounds + My Presets
Main     MACHN LEVEL LTNCY LOAD(ro)                 <- MACHN = 15 synth machines
SYN      the current machine's 8 controls, hardware labels, swaps with MACHN
AMP      ATK HOLD DEC REL | DIST VOL PAN PORT       <- envelope picture on the first row
FILT     BASE WDTH HPQ LPQ | ATK DEC BOFS WOFS
EFX      EQF EQG SRR DTIM | DSND DFB DBAS DWID
LFO1..3  PAGE DEST TRIG WAVE | MULT SPD INTL DPTH   <- DEST shows the target's name for the chosen PAGE
```

- Hardware 4-character labels; the held-knob header spells the full name.
- Bipolar knobs (TUNE, PAN, DIST, BOFS, EQG, ...) read -64..+63 as on the LCD.
- List-type controls are real pickers (SID WAVE: TRI/SAW/PULS/MIX/NOIS; FM ratios; DPRO waves;
  VO-6 consonants).
- Unused hardware slots ("-") are blank cells, not dead knobs.
- Switching machine swaps the SYN page and restores that machine's last values (as upstream One does).
- **Decided (Josh): no effects in One.** The 7 FX machines are only in Monomodule FX; to process a One
  sound through one, put Monomodule FX after it.

## 3. Monomodule FX — the pages

```
Presets  browser
Main     MACHN LEVEL LTNCY LOAD(ro)                 <- MACHN = THRU REVERB CHORUS DYNAMIX RINGMOD PHASER FLANGER
SYN      the FX machine's controls (e.g. REVERB: DEC DAMP GATE MIX HP LP - INP)
AMP      held open as on the hardware; DIST VOL PAN still useful
FILT / EFX / LFO1..3   as in One
```

## 4. Presets

- Schwung's **My Presets** works for both (state = one track, self-contained).
- **Import**: a folder for the user's Monomachine `.syx` dumps; the Presets browser lists every sound in
  them (filter: current machine / all). FX lists only FX-machine sounds.
- None bundled upstream; ship an **Init** per machine and a few sounds made on the Move.

## 5. v1 scope vs later

**v1**: One + FX with the pages above, machine-following SYN pages, pickers, bipolar readouts, LFO pages,
LOAD readout, My Presets, `.syx` import browser.
**Later**: velocity / mod-wheel assignment (the hardware's ASSIGN page — needs engine work), polyphony
(several engines per instance — gated on CM4 numbers), `.syx` export.

## Appendix: implementation notes

- A sound generator's `ui_hierarchy` is served only from `get_param("ui_hierarchy")`
  (schwung-current `docs/MODULES.md` ~1975); an audio FX may declare it in module.json. Keep
  module.json `chain_params` generated from the same table (it feeds modulation/patch metadata).
- **Machine-following pages, the DR32 way**: one small SYN level per machine, gated with `visible_if` on
  the current machine (15 levels in One, 7 in FX, ~15 KB served), per-machine unique keys (`sid_wave`,
  `fmp_1frq`, ...) the DSP aliases onto the 8 raw SYN slots. DR32's `tools/gen_engine_ui.mjs` is the
  generator pattern. Unique keys everywhere (memory hierarchy-duplicate-key-kills-per-voice-sends).
- Bipolar: `int min:-64 max:63`, offset on the wire. Lists: `type:"enum"` + `options`; index <-> raw via
  upstream `SpecData` (`libs/monomodule/src/plugin/one/SpecData.cpp`). Labels/defaults/bipolar masks:
  `libs/monomodule/src/core/host/Machines.h`. Blank slots: `""` in `knobs`.
- LFO DEST name: a `custom:` knob cell (32x15) drawn by `canvas.js`, served in the plugin's
  `chain_params` (memory schwung-custom-knob-widgets). LFO params exist host-side
  (`HostModel::setLfoParam`) but are not yet exposed by `src/plugin/mnm_plugin.cpp`.
- AMP viz: `viz: {group:"amp", role: attack|hold|decay|release}` on one row of four.
- LOAD: `stats[0].lastUs / 2902 us`, served live. Overload -> `get_error`.
- `.syx` import: upstream `libs/monomodule/src/core/library/MnmDump.h` (`Kit`, `KitTrack`), dedup via
  `Catalog.h`; browser = DR32's list/items pattern, scanned off the audio callback.
