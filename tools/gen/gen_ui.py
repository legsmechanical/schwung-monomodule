#!/usr/bin/env python3
"""Generates the Monomodule UI from upstream's display spec (tools/gen/spec.json, made by spec_dump.cpp).

Writes:
  src/plugin/generated/mnm_ui.h      parameter tables for the plugin + the served ui_hierarchy (both variants)
  modules/monomodule-one/module.json chain_params (the host's modulation / patch metadata)
  modules/monomodule-fx/module.json  chain_params + ui_hierarchy (an audio FX's hierarchy is read from module.json)

The pages follow the selected machine: one SYN level per machine, gated with visible_if on `machine`.
LFO DEST names depend on that LFO's PAGE: one DEST param per page value, each gated on the PAGE param.

  python3 tools/gen/gen_ui.py [--check]
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = json.loads((ROOT / "tools/gen/spec.json").read_text())

PAGES = ["syn", "amp", "filt", "efx"]          # engine page index 0..3; LFOs are 4..6
KIND = {"numeric": 0, "bipolar": 1, "list": 2, "readout": 3, "synt": 4}
SYNT_PLACEHOLDERS = [f"@S{i}@" for i in range(8)]


def digibank_names():
    """DDRW / DENS pick from the 64 Digibank slots. The factory bank is not in the OS file; upstream builds
    a stand-in (libs/monomodule/src/core/dsp/Digibank.cpp): slots 1-32 are the DPRO-WAVE waveforms (no names
    anywhere, so they stay D01-D32), slots 33-64 classic shapes it names itself. Read from that source so
    the labels always match the bank the engine loads."""
    src = (ROOT / "libs/monomodule/src/core/dsp/Digibank.cpp").read_text()
    body = src[src.index("struct Shape"):]
    names = re.findall(r'\{"([A-Z0-9]+)",', body)
    assert len(names) == 32, f"expected 32 named stand-in shapes, found {len(names)}"
    return [f"D{i + 1:02d}" for i in range(32)] + names


DIGIBANK = digibank_names()
for _m in SPEC["machines"]:   # Digibank slots, everywhere they are listed: the stand-in's names where it has them
    for _p in _m["params"]:
        if _p.get("values") and len(_p["values"]) == 64 and _p["values"][0] == "D01":
            _p["values"] = DIGIBANK


def machine_label(m):
    if m["group"] == "FX": return m["name"]
    if m["group"] == m["name"]: return m["name"]          # GND
    if m["group"] == "VO": return m["name"]               # VO-6
    return f'{m["group"]} {m["name"]}'


def slug(s):
    return re.sub(r"[^a-z0-9]", "", s.lower().replace("+", "p"))


PREFIX = {"GND": "gnd", "SIN": "sin", "NOIS": "nois", "6581": "sid", "SAW": "saw", "PULS": "puls", "ENS": "ens",
          "WAVE": "wave", "BBOX": "bbox", "DDRW": "ddrw", "DENS": "dens", "STAT": "fms", "PAR": "fmp", "DYN": "fmd",
          "VO-6": "vo6", "THRU": "thru", "REVERB": "rev", "CHORUS": "cho", "DYNAMIX": "dyn", "RINGMOD": "ring",
          "PHASER": "pha", "FLANGER": "fla"}


# Full names for the header while a knob is held (the cell keeps the hardware label as short_name).
# From the Monomachine user manual (OS 1.32, Appendix A and the track pages), which defines each label as
# "LABEL (name)". Keyed by machine prefix for machine-specific meanings, then by page, then generic.
LONG = {
    "": {"TUNE": "Tune", "PW": "Pulse Width", "PWAD": "Pulse Width Add", "PWRS": "Pulse Width Restart",
         "WAVE": "Waveform", "UNIL": "Unison Level", "UNIW": "Unison Width", "UNIX": "Unison Extended Level",
         "SUBX": "Square Sub -1 Oct", "SUB1": "Sine Sub -1 Oct", "SUB2": "Sine Sub -2 Oct",
         "PCH2": "Pitch 2", "PCH3": "Pitch 3", "PCH4": "Pitch 4", "CHRL": "Chorus Level", "CHRW": "Chorus Width",
         "WP": "Wave Phase", "WPM": "Wave Phase Mod", "WPRS": "Wave Phase Restart", "SYNC": "Hard Sync",
         "SFRQ": "Sync Source Freq", "PTCH": "Pitch", "STRT": "Sample Start", "RTRG": "Retrig", "RTIM": "Retrig Timing",
         "WAV1": "Waveform 1", "WAV2": "Waveform 2", "TIME": "Glide Time", "BR1": "Bit Reduction 1",
         "BR2": "Bit Reduction 2", "TONE": "Tone", "INP": "Input Gain", "MIX": "Dry/Wet Mix", "DEP": "Depth",
         "SPD": "Speed", "FB": "Feedback", "WID": "Stereo Width", "DEL": "Delay", "LP": "Low-Pass Filter",
         "HP": "High-Pass Filter"},
    "nois": {"ST": "Stereo", "RED": "Redness", "STON": "Stereo On"},
    "sid": {"MOD": "Modulation", "MSRC": "Modulation Source", "MFRQ": "Modulation Freq"},
    "ddrw": {"MIX": "Waveform Mix", "WID": "Waveform Distance"},
    "fms": {"1FRQ": "Mod 1 Frequency", "1FIN": "Mod 1 Fine Tune", "1ENV": "Mod 1 Volume & Env", "1FB": "Mod 1 Feedback",
            "2FRQ": "Mod 2 Frequency", "2VOL": "Mod 2 Vol & Feedback"},
    "fmp": {"1FRQ": "Mod 1 Frequency", "1ENV": "Mod 1 Volume & Env", "2FRQ": "Mod 2 Frequency",
            "2ENV": "Mod 2 Volume & Env", "3FRQ": "Mod 3 Frequency", "3ENV": "Mod 3 Volume & Env"},
    "fmd": {"1FRQ": "Mod 1 Frequency", "1FEN": "Mod 1 Freq Envelope", "1VOL": "Mod 1 Vol & Feedback",
            "1VEN": "Mod 1 Vol Envelope", "2FRQ": "Mod 2 Frequency", "2ENV": "Mod 2 Volume & Env", "2FB": "Mod 2 Feedback"},
    "vo6": {"VOC1": "Vowel Formant 1", "VOC2": "Vowel Formant 2", "V-SW": "Vowel Switch", "VOIC": "Voice Type",
            "CONS": "Consonant", "CLEN": "Consonant Length", "CVOL": "Consonant Volume"},
    "rev": {"DEC": "Decay", "DAMP": "Damping", "GATE": "Gate Sensitivity"},
    "dyn": {"ATK": "Attack", "REL": "Release", "THRS": "Threshold", "RAT": "Ratio", "GAIN": "Makeup Gain",
            "RMS": "RMS Detection"},
    "ring": {"WAVE": "Waveform (Sine-Tri)", "EXT": "External Signal"},
    "pha": {"CNTR": "Center"},
    "amp": {"ATK": "Attack", "HOLD": "Hold", "DEC": "Decay", "REL": "Release", "DIST": "Distortion", "VOL": "Volume",
            "PAN": "Pan", "PORT": "Portamento"},
    "filt": {"BASE": "Filter Base", "WDTH": "Filter Width", "HPQ": "High-Pass Q", "LPQ": "Low-Pass Q",
             "ATK": "Env Attack", "DEC": "Env Decay", "BOFS": "Base Env Offset", "WOFS": "Width Env Offset"},
    "efx": {"EQF": "EQ Frequency", "EQG": "EQ Gain", "SRR": "Sample Rate Reduction", "DTIM": "Delay Time",
            "DSND": "Delay Send", "DFB": "Delay Feedback", "DBAS": "Delay Filter Base", "DWID": "Delay Filter Width"},
    "lfo": {"PAGE": "Target Page", "DEST": "Destination", "TRIG": "Trig Mode", "WAVE": "Waveform",
            "MULT": "Speed Multiplier", "SPD": "Speed", "INTL": "Interlace", "DPTH": "Depth"},
}


def long_name(scope, label):
    name = LONG.get(scope, {}).get(label) or LONG[""].get(label)
    assert name, f"no long name for {scope}/{label}"
    return name


# Widgets, decided per control (docs: schwung-current docs/MODULES.md "Parameter visualisations"):
#  - a picture only where it is TRUE: the host's waveform silhouettes know TRI/SAW/SQR/RND and draw a sine
#    for anything else, so SID WAVE (PULS, MIX) and LFO WAVE (ITRI, EXP, RMP, ...) show their names instead
#  - FILT ATK+DEC sit side by side: an attack/decay envelope. AMP's ATK HOLD DEC REL cannot be one (HOLD has
#    no role and a group must be contiguous)
#  - levels are faders; OFF/ON lists are switches (detected); small ranges draw as big numbers (automatic)
VIZ = {("sid", "WAVE"): False, ("lfo", "WAVE"): False,
       ("filt", "ATK"): {"group": "fenv", "role": "attack"}, ("filt", "DEC"): {"group": "fenv", "role": "decay"},
       ("amp", "VOL"): {"kind": "fader"}, ("dyn", "GAIN"): {"kind": "fader"},
       # envelope and glide TIMES, not levels: the detector's fader guess is wrong for them
       ("amp", "ATK"): False, ("amp", "HOLD"): False, ("amp", "DEC"): False, ("amp", "REL"): False,
       ("amp", "PORT"): False}


def param_entry(key, label, p, scope=""):
    """A chain_params / hierarchy param for one hardware knob. Values travel as the display value:
    numeric 0..127, bipolar -64..63, lists / readouts as their option names."""
    e = {"key": key, "name": long_name(scope, label), "short_name": label}
    v = VIZ.get((scope, label))
    if v is None and p.get("values") == ["OFF", "ON"]: v = {"kind": "switch"}   # an on/off: say so
    if v is not None: e["viz"] = v
    if p["display"] in ("list", "readout"):
        e["type"] = "enum"
        e["options"] = p["values"]
        e["options_as_string"] = True   # names on the wire; some lists read like numbers (ENS "-12", "+01")
    elif p["display"] == "bipolar":
        e.update(type="int", min=-64, max=63)
    else:
        e.update(type="int", min=0, max=127)
    return e


def build(variant):
    fx = variant == "fx"
    machines = [m for m in SPEC["machines"] if m["fx"] == fx]
    defs = []        # (key, page, index, kind, count, machine, values) for the plugin table
    chain = []       # chain_params
    levels = {}
    nav = []
    compat = {"syn": {}, "dest": {}}   # pieces of the 1.4.0-compatible (gate-free) hierarchy

    labels = [machine_label(m) for m in machines]
    chain.append({"key": "machine", "name": "Machine", "short_name": "MACHN", "type": "enum", "options": labels,
                  "default": labels[0], "options_as_string": True})
    chain.append({"key": "level", "name": "Track Level", "short_name": "LEVEL", "type": "int", "min": 0, "max": 127,
                  "default": 100, "viz": {"kind": "fader"}})
    chain.append({"key": "depth", "name": "Latency (blocks ahead)", "short_name": "LTNCY", "type": "int", "min": 1,
                  "max": 4, "default": 2})
    chain.append({"key": "load", "name": "Engine Load", "short_name": "LOAD", "type": "int", "min": 0, "max": 100,
                  "unit": "%", "access": "read", "live": True})

    # one SYN level per machine, gated on the machine
    for m, label in zip(machines, labels):
        pre = PREFIX[m["name"]]
        knobs, params = [], []
        for i, p in enumerate(m["params"]):
            if p["display"] == "blank":
                knobs.append("")
                continue
            key = f'{pre}_{slug(p["label"])}'
            e = param_entry(key, p["label"], p, pre)
            chain.append(e); params.append(e); knobs.append(key)
            defs.append((key, 0, i, KIND[p["display"]], p["count"], m["index"], p.get("values"), p["default"]))
        while knobs and knobs[-1] == "": knobs.pop()
        lv = f"syn_{pre}"
        gate = {"param": "machine", "equals": label}
        levels[lv] = {"name": label, "visible_if": gate, "params": params, "knobs": knobs}
        compat["syn"][m["index"]] = {"name": label, "params": params, "knobs": knobs}
        nav.append({"level": lv, "label": label, "visible_if": gate})

    # AMP / FILT / EFX
    for pi, sp in enumerate(SPEC["shared"]):
        page = PAGES[pi + 1]
        knobs, params = [], []
        for i, lab in enumerate(sp["labels"]):
            bip = (sp["bipolarMask"] >> i) & 1
            key = f"{page}_{slug(lab)}"
            e = param_entry(key, lab, {"display": "bipolar" if bip else "numeric"}, page)
            chain.append(e); params.append(e); knobs.append(key)
            dflt = SPEC["ampDefaultsFx"][i] if (fx and pi == 0) else sp["defaults"][i]
            defs.append((key, pi + 1, i, KIND["bipolar" if bip else "numeric"], 128, -1, None, dflt))
        levels[page] = {"name": sp["name"], "params": params, "knobs": knobs}
        nav.append({"level": page, "label": sp["name"]})

    # LFO 1-3: DEST is one param per PAGE value, gated on the PAGE
    pages = SPEC["lfo"][0]["values"]
    for n in range(1, 4):
        knobs, params = [], []
        for i, p in enumerate(SPEC["lfo"]):
            if p["label"] == "DEST":
                for pg, pname in enumerate(pages):
                    key = f"lfo{n}_dest_{slug(pname)}"
                    # SYNT: the current machine's own labels, filled in by the plugin when it serves the page
                    # (placeholders @S0@..@S7@); module.json keeps the generic PAR1-8 as the static fallback.
                    vals = SYNT_PLACEHOLDERS if pname == "SYNT" else SPEC["lfoDest"][pg]
                    e = param_entry(key, "DEST", {"display": "list", "values": vals}, "lfo")
                    chain.append(dict(e))
                    compat["dest"][(n, pg)] = dict(e)
                    e["visible_if"] = {"param": f"lfo{n}_page", "equals": pname}
                    params.append(e); knobs.append(key)
                    defs.append((key, 3 + n, i, KIND["synt"] if pname == "SYNT" else KIND["list"], 8, -1,
                                 SPEC["lfoDest"][pg], p["default"]))
                continue
            key = f"lfo{n}_{slug(p['label'])}"
            e = param_entry(key, p["label"], p, "lfo")
            chain.append(e); params.append(e); knobs.append(key)
            defs.append((key, 3 + n, i, KIND[p["display"]], p["count"], -1, p.get("values"), p["default"]))
        levels[f"lfo{n}"] = {"name": f"LFO{n}", "params": params, "knobs": knobs}
        nav.append({"level": f"lfo{n}", "label": f"LFO{n}"})

    root_params = [c for c in chain[:4]] + nav
    title = "Monomodule FX" if fx else "Monomodule One"
    levels = {"root": {"name": title, "list_param": "preset", "count_param": "preset_count", "name_param": "preset_name",
                       "params": root_params, "knobs": ["machine", "level", "depth", "load"]}, **levels}
    keys = [c["key"] for c in chain]
    dupes = {k for k in keys if keys.count(k) > 1}
    assert not dupes, f"duplicate keys: {dupes}"   # a repeated key makes the host drop ALL metadata
    # The 1.4.0-compatible hierarchy has no gates: ONE syn level holding the current machine's page and each
    # LFO's DEST entry for its current PAGE, filled in by the plugin (@SYNLEVEL@, @DPn@ entry, @DKn@ key)
    # and re-served on an is_loading edge whenever they change. Host 1.4.0 re-reads gates only for knob
    # writes (charlesvestal/schwung#533 came after it), so a preset or state could not move a gate there.
    clevels = {}
    for name, lv in levels.items():
        if name.startswith("syn_"): continue
        if name.startswith("lfo"):
            n = int(name[3])
            ps = [q for q in lv["params"] if not q["key"].startswith(f"lfo{n}_dest_")]
            ks = [k for k in lv["knobs"] if not k.startswith(f"lfo{n}_dest_")]
            ps.insert(1, f"@DP{n}@"); ks.insert(1, f"@DK{n}@")
            clevels[name] = {"name": lv["name"], "params": ps, "knobs": ks}
        else:
            clevels[name] = lv
    croot = dict(levels["root"])
    croot["params"] = [q for q in croot["params"] if not (isinstance(q, dict) and q.get("level", "").startswith("syn_"))]
    croot["params"].insert(4, {"level": "syn", "label": "@M@"})
    clevels["root"] = croot
    clevels = {"root": croot, "syn": "@SYNLEVEL@", **{k: v for k, v in clevels.items() if k != "root"}}
    compat["hierarchy"] = {"levels": clevels}
    return {"levels": levels}, chain, defs, [(m["index"], l) for m, l in zip(machines, labels)], compat


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def header(variants):
    out = ["// GENERATED by tools/gen/gen_ui.py from upstream's SpecData (tools/gen/spec.json). Do not edit.",
           "#pragma once", "#include <cstdint>", "", "namespace mnm::ui {", "",
           "struct ParamDef {",
           "    const char* key;",
           "    uint8_t page;       // 0 SYN 1 AMP 2 FILT 3 EFX 4..6 LFO1..3",
           "    uint8_t index;      // knob 0..7 on that page",
           "    uint8_t kind;       // 0 numeric 0..127, 1 bipolar -64..63, 2 list (uneven buckets), 3 readout (one name per raw),",
           "                        // 4 LFO DEST on the SYNT page: 8 names = the current machine's SYN labels",
           "    uint8_t count;      // options of a list / readout",
           "    int16_t machine;    // SYN params: the machine they belong to (host::Machine), else -1",
           "    uint8_t defaultRaw;",
           "    const char* const* values;",
           "};",
           "struct MachineDef { int16_t id; const char* label; };",
           "struct SynLabels { int16_t id; const char* labels[8]; };   // nullptr = a blank hardware slot",
           "struct MachineJson { int16_t id; const char* json; };", ""]
    for v, (hier, chain, defs, machines, compat) in variants.items():
        V = v.upper()
        vals = {}
        for d in defs:
            if d[6] is not None:
                name = f"k{V}Vals_{d[0]}"
                vals[d[0]] = name
                out.append(f"inline const char* const {name}[] = {{{', '.join(cstr(x) for x in d[6])}}};")
        out.append(f"inline const ParamDef k{V}Params[] = {{")
        for key, page, index, kind, count, machine, values, dflt in defs:
            out.append(f"    {{{cstr(key)}, {page}, {index}, {kind}, {count if count < 128 else 0}, {machine}, {dflt}, "
                       f"{vals.get(key, 'nullptr')}}},")
        out.append("};")
        out.append(f"constexpr int k{V}ParamCount = {len(defs)};")
        out.append(f"inline const MachineDef k{V}Machines[] = {{{', '.join(f'{{{i}, {cstr(l)}}}' for i, l in machines)}}};")
        rows = []
        for mid, _ in machines:
            m = next(x for x in SPEC["machines"] if x["index"] == mid)
            labs = [("nullptr" if q["display"] == "blank" else cstr(q["label"])) for q in m["params"]]
            rows.append(f"{{{mid}, {{{', '.join(labs)}}}}}")
        out.append(f"inline const SynLabels k{V}SynLabels[] = {{{', '.join(rows)}}};")
        cjs = json.dumps(chain, separators=(",", ":"))
        out.append(f"inline const char k{V}ChainParams[] = {cstr(cjs)};")
        out.append(f"constexpr int k{V}MachineCount = {len(machines)};")
        js = json.dumps(hier, separators=(",", ":"))
        out.append(f"inline const char k{V}Hierarchy[] = {cstr(js)};")
        # 1.4.0-compatible pieces: template (quoted placeholders become raw JSON), SYN level per machine,
        # DEST entry per (LFO, PAGE)
        cjs = json.dumps(compat["hierarchy"], separators=(",", ":"))
        cjs = cjs.replace('"@SYNLEVEL@"', "@SYNLEVEL@")
        for n in range(1, 4): cjs = cjs.replace(f'"@DP{n}@"', f"@DP{n}@")
        out.append(f"inline const char k{V}CompatHierarchy[] = {cstr(cjs)};")
        out.append(f"inline const MachineJson k{V}CompatSyn[] = {{" + ", ".join(
            f"{{{mid}, {cstr(json.dumps(compat['syn'][mid], separators=(',', ':')))}}}" for mid, _ in machines) + "};")
        out.append(f"inline const char* const k{V}CompatDest[3][9] = {{" + ", ".join(
            "{" + ", ".join(cstr(json.dumps(compat['dest'][(n, pg)], separators=(',', ':'))) for pg in range(9)) + "}"
            for n in range(1, 4)) + "};")
        out.append(f"constexpr int k{V}HierarchyLen = {len(js)};")
        out.append("")
    out.append("} // namespace mnm::ui")
    return "\n".join(out) + "\n"


def module_json_text(j):
    """module.json with one parameter per line: readable, and far under the host's 64 KB file limit
    (chain_params.c refuses a larger module.json, and then the modulation / patch metadata is empty)."""
    compact = lambda x: json.dumps(x, separators=(", ", ": "))
    caps = j["capabilities"]
    lines = ["{"]
    top = [k for k in j if k != "capabilities"]
    for k in top:
        lines.append(f'  "{k}": {json.dumps(j[k], indent=2).replace(chr(10), chr(10) + "  ")},')
    lines.append('  "capabilities": {')
    ck = list(caps)
    for i, k in enumerate(ck):
        end = "," if i < len(ck) - 1 else ""
        v = caps[k]
        if k == "chain_params":
            lines.append('    "chain_params": [')
            lines += [f"      {compact(e)}{',' if n < len(v) - 1 else ''}" for n, e in enumerate(v)]
            lines.append(f"    ]{end}")
        elif k == "ui_hierarchy":
            lines.append('    "ui_hierarchy": {"levels": {')
            lv = v["levels"]
            for n, (name, level) in enumerate(lv.items()):
                lines.append(f'      "{name}": {compact(level)}{"," if n < len(lv) - 1 else ""}')
            lines.append(f"    }}}}{end}")
        else:
            lines.append(f'    "{k}": {compact(v)}{end}')
    lines.append("  }")
    lines.append("}")
    text = "\n".join(lines) + "\n"
    json.loads(text)   # must still parse
    assert len(text.encode()) < 60000, f"module.json {len(text)} bytes: too close to the host's 64 KB limit"
    return text


def main():
    check = "--check" in sys.argv
    variants = {"one": build("one"), "fx": build("fx")}
    outputs = {ROOT / "src/plugin/generated/mnm_ui.h": header(variants)}
    for v, mod in (("one", "monomodule-one"), ("fx", "monomodule-fx")):
        path = ROOT / f"modules/{mod}/module.json"
        j = json.loads(path.read_text())
        hier, chain, _, _, _ = variants[v]
        j["capabilities"]["chain_params"] = [
            dict(c, options=SPEC["lfoDest"][1]) if c.get("options") == SYNT_PLACEHOLDERS else c for c in chain]
        os_asset = j["assets"] if isinstance(j["assets"], dict) else j["assets"][0]
        j["assets"] = [os_asset, {
            "path": "dumps", "label": "Monomachine sysex dumps", "extensions": [".syx"], "optional": True,
            "description": "Optional: kit dumps from a Monomachine (.syx). Every sound in them appears in the Presets "
                           "browser of Monomodule One (synth sounds) and Monomodule FX (FX sounds)."}]
        # Both modules SERVE their hierarchy (get_param), so the SYNT DEST names can follow the machine. An
        # audio FX would read a module.json hierarchy first and never ask, so it must not carry one.
        j["capabilities"].pop("ui_hierarchy", None)
        outputs[path] = module_json_text(j)
    stale = []
    for path, text in outputs.items():
        if not path.exists() or path.read_text() != text:
            stale.append(path)
            if not check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
    for v, (hier, chain, defs, machines, _) in variants.items():
        print(f"{v}: {len(machines)} machines, {len(defs)} engine params, {len(chain)} chain params, "
              f"hierarchy {len(json.dumps(hier, separators=(',', ':')))} bytes")
    for p in outputs:
        if p.suffix == ".json": print(f"{p.relative_to(ROOT)}: {p.stat().st_size if p.exists() else 0} bytes")
    if check and stale:
        print("STALE:", *[str(p.relative_to(ROOT)) for p in stale]); sys.exit(1)


if __name__ == "__main__":
    main()
