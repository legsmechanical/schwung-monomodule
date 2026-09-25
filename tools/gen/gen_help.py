#!/usr/bin/env python3
"""Writes modules/<module>/help.json (the host's Help viewer, and the 'Module Help' row on the Module page).

Text is written as paragraphs here and wrapped to the viewer's ~22-character lines. Machine descriptions
follow the Monomachine user manual (OS 1.32, Appendix A); the DPRO BBOX key map is the manual's table.
  python3 tools/gen/gen_help.py
"""
import json
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WIDTH = 22


def lines(*paras):
    out = []
    for i, p in enumerate(paras):
        if i: out.append("")
        if isinstance(p, list):          # preformatted rows (tables)
            out += p
        else:
            out += textwrap.wrap(p, WIDTH) or [""]
    return out


def topic(title, *paras):
    return {"title": title, "lines": lines(*paras)}


SETUP = topic("Setup",
    "Needs the Monomachine OS 1.32B file, a free download from Elektron: "
    "Elektron_SFX6-60_OS1.32B.syx. Upload it on this module's page in the web manager (one upload serves "
    "both Monomodule One and Monomodule FX).",
    "Optional: upload .syx kit dumps from a Monomachine to the 'dumps' folder. Every sound in them "
    "appears in the Presets browser.")

PAGES_ONE = topic("Pages",
    "Presets: an Init for each machine, then the sounds of your dumps.",
    "Main: MACHN picks the machine, LEVEL the track level, LTNCY how many blocks the engine renders "
    "ahead (1 is lowest latency, 2 is safest), LOAD shows the engine's CPU use.",
    "Then the machine's own page (its name is the page title), AMP, FILT, EFX and LFO 1-3, as on the "
    "Monomachine. Values are the hardware's 0-127, bipolar ones -64..+63.",
    "Hold a knob to see its full name.")

PAGES_FX = topic("Pages",
    "Presets: an Init for each FX machine, then the FX sounds of your dumps.",
    "Main: MACHN picks the effect, LEVEL the output level, LTNCY the blocks rendered ahead, LOAD the "
    "engine's CPU use.",
    "Then the effect's own page, AMP (its envelope stays open), FILT, EFX and LFO 1-3.")

LFO = topic("LFOs",
    "PAGE picks which page the LFO modulates, DEST the control on it. With PAGE on SYNT the "
    "destinations are the current machine's own controls.",
    "TRIG: FREE runs freely, TRIG restarts on each note, HOLD samples, ONE plays one cycle, HALF half a "
    "cycle. MULT and SPD set the speed, INTL interlaces, DPTH is the depth.")

SYNTH_MACHINES = [
    topic("GND", "GND is the empty machine: no sound of its own. GND SIN is a pure sine; GND NOIS is noise "
          "with a sample-and-hold (ST), redness (RED) and a stereo switch (STON)."),
    topic("SWAVE", "SuperWave: warm, thick analogue-style sounds.",
          "SAW: a saw with unison oscillators (UNIL level, UNIW width, UNIX extended unison) and three "
          "sub oscillators (SUBX square, SUB1 and SUB2 sines, one and two octaves down).",
          "PULS: the same idea with pulse waves and pulse width (PW, PWAD, PWRS).",
          "ENS: an ensemble of up to four pitches (PCH2-4 add intervals to the played note) with chorus "
          "(CHRL, CHRW): chords and textures."),
    topic("SID 6581", "Models the C64's SID chip: pulse width (PW, PWAD, PWRS), waveform (WAVE), ring "
          "modulation and hard sync (MOD) from a source (MSRC) at a frequency (MFRQ)."),
    topic("DPRO WAVE", "A crunchy wavetable synth on 32 original 12-bit waveforms (WAVE). WP morphs toward "
          "the next waveform, WPM sweeps it, WPRS restarts the sweep on each note. SYNC hard-syncs to SFRQ."),
    topic("DPRO BBOX", "BeatBox: 24 percussive 12-bit samples, chosen by the key you play (repeated in "
          "every octave). PTCH tunes, STRT sets the sample start, RTRG and RTIM retrigger.",
          ["C  Bass Drum 1", "C# Snare Drum 1", "D  Tom 1", "D# Tom 2", "E  Bongo Congo", "F  Clap",
           "F# Rim Shot", "G  Cow Bell", "G# Closed HiHat", "A  Open HiHat", "A# Ride Cymbal", "B  Crash Cymbal",
           "", "an octave up:", "C  Bass Drum 2", "C# Snare Drum 2", "D  Timbale", "D# Aggobell", "E  Timpani",
           "F  Snap", "F# Wood", "G  Triangle", "G# Shaker", "A  Maracas", "A# Whistle", "B  Blipp"]),
    topic("DPRO DDRW", "DoubleDraw: two waveforms (WAV1, WAV2) mixed (MIX), each with bit reduction (BR1, BR2) "
          "and a pitch distance between them (WID). TIME glides between waveform changes.",
          "D01-D32 are the DPRO WAVE waveforms, D33-D64 classic shapes (the real Digibank is not in the OS "
          "file; Monomodule fills it with these)."),
    topic("DPRO DENS", "An ensemble (up to four pitches, PCH2-4) on one Digibank waveform (WAVE), with chorus "
          "(CHRL, CHRW)."),
    topic("FM+", "Three FM machines.",
          "STAT: two modulators with listed ratios (1FRQ, 2FRQ), fine tune, envelopes and feedback.",
          "PAR: three parallel modulators (1-3FRQ with their volume/envelope 1-3ENV).",
          "DYN: the wildest, with continuous frequencies and envelopes on frequency and volume.",
          "TONE controls the overall brightness."),
    topic("VO-6", "Speech synthesis. VOC1 and VOC2 move the vowel formants, CONS picks a consonant (CLEN "
          "its length, CVOL its volume), VOIC the voice type, V-SW switches vowels."),
]

FX_MACHINES = [
    topic("THRU", "Passes the input through the track's AMP, FILT and EFX pages: use it to filter, "
          "distort or delay audio. INP sets the input gain."),
    topic("REVERB", "DEC decay, DAMP damping, GATE gate sensitivity, MIX dry/wet, HP and LP filter the reverb."),
    topic("CHORUS", "DEL delay, DEP depth, SPD speed, FB feedback, WID stereo width, LP low-pass."),
    topic("DYNAMIX", "A compressor: ATK, REL, THRS threshold, RAT ratio, GAIN makeup gain, RMS detection, "
          "MIX dry/wet."),
    topic("RINGMOD", "Ring-modulates the input with a waveform from sine to triangle (WAVE); EXT the amount "
          "of input, MIX dry/wet."),
    topic("PHASER", "CNTR centre, DEP depth, SPD speed, FB feedback, WID stereo width, MIX dry/wet."),
    topic("FLANGER", "DEL delay, DEP depth, SPD speed, FB feedback, WID stereo width, MIX dry/wet."),
]

ABOUT = topic("About",
    "Monomodule runs the Monomachine's own sound engine inside an emulated DSP chip, so it is the "
    "hardware's sound, not an imitation. Monomodule is by shnolk (GPL-3.0); this is its Schwung port.",
    "Not affiliated with, endorsed by or sponsored by Elektron.")

HELP = {
    "monomodule-one": {"title": "Monomodule One", "children": [
        topic("Overview", "One Elektron Monomachine track: 15 synth machines played from the keys, with the "
              "hardware's pages. Mono, like each Monomachine track."),
        SETUP, PAGES_ONE, LFO, {"title": "Machines", "children": SYNTH_MACHINES}, ABOUT]},
    "monomodule-fx": {"title": "Monomodule FX", "children": [
        topic("Overview", "The Monomachine's seven FX machines as an audio effect on the audio in front of it."),
        SETUP, PAGES_FX, LFO, {"title": "Effects", "children": FX_MACHINES}, ABOUT]},
}

for mod, doc in HELP.items():
    path = ROOT / "modules" / mod / "help.json"
    path.write_text(json.dumps(doc, indent=1) + "\n")
    for t in doc["children"]:
        for l in t.get("lines", []) + [x for c in t.get("children", []) for x in c.get("lines", [])]:
            assert len(l) <= WIDTH, f"{mod}: line too long: {l!r}"
    print(f"{path.relative_to(ROOT)}: {path.stat().st_size} bytes")
