#!/usr/bin/env python3
"""On-device test of Monomodule One (and FX behind it) on track 1 of a Move running stock Schwung.
Notes are sent as external USB MIDI on channel 1 (cable 2), which Schwung routes straight to the slot.
Loads the modules itself; restores the slot's previous synth / fx1 at the end."""
import json, os, signal, subprocess, sys, time
from move import Device, ssh

MACHINES = ["FM+ STAT","FM+ PAR","FM+ DYN","GND ---","GND SIN","GND NOIS","SWAVE SAW","SWAVE PULS","SWAVE ENS",
            "SID 6581","DPRO WAVE","DPRO BBOX","DPRO DDRW","DPRO DENS","VO-6"]
FX = ["THRU","REVERB","CHORUS","DYNAMIX","RINGMOD","PHASER","FLANGER"]

def status(b, prefix="synth"):
    return json.loads(b.get_param(f"{prefix}:status", overtake=False))

def note(b, n, on):
    b.inject_midi(bytes([0x29, 0x90, n, 100]) if on else bytes([0x28, 0x80, n, 0]))

def wait_ready(b, prefix, timeout=20):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = status(b, prefix)
            if s.get("phase") == 3: return time.time() - t0, s
        except Exception: pass
        time.sleep(0.2)
    raise SystemExit(f"{prefix} not ready after {timeout} s")

def main():
    fails = []
    with Device() as d:
        b = d.bus
        prev_synth = b.get_param("synth_module", overtake=False)
        if prev_synth == "monomodule-one":   # left loaded by an interrupted run: restore to empty
            prev_synth = os.environ.get("MNM_RESTORE_SYNTH", "")
        prev_fx = b.get_param("fx1_module", overtake=False)
        print(f"slot 0 before: synth={prev_synth!r} fx1={prev_fx!r}")
        if prev_synth != "monomodule-one":
            b.set_param("synth:module", "monomodule-one", overtake=False)
        t, s = wait_ready(b, "synth"); print(f"ready in {t:.1f} s")

        print("\n-- every synth machine: a 0.6 s note")
        for m in MACHINES:
            b.set_param("synth:machine", m, overtake=False); time.sleep(0.15); status(b)
            note(b, 48, True); time.sleep(0.6); s = status(b); note(b, 48, False); time.sleep(0.15)
            silent_by_design = m == "GND ---"   # the Monomachine's empty machine
            ok = (s["peak"] == 0 if silent_by_design else s["peak"] > 0) and s["underruns"] == 0 and not s["faulted"]
            if not ok: fails.append(m)
            print(f"  {m:11s} peak {s['peak']:5d}  underruns {s['underruns']}  last {s['last_us']:4d} us  max {s['max_us']:4d} us  {'ok' if ok else 'FAIL'}")

        for depth in (1, 2):
            b.set_param("synth:depth", str(depth), overtake=False)
            b.set_param("synth:machine", "SID 6581", overtake=False); time.sleep(0.2)
            u0 = status(b)["underruns"]
            t0 = time.time(); n = 0
            while time.time() - t0 < 20:   # 20 s of notes, retriggered every 150 ms, machine switch every 2 s
                note(b, 40 + n % 24, True); time.sleep(0.12); note(b, 40 + n % 24, False); time.sleep(0.03)
                n += 1
                if n % 13 == 0: b.set_param("synth:machine", MACHINES[n % len(MACHINES)], overtake=False)
            s = status(b)
            print(f"\n-- latency {depth}: 20 s, {n} notes, machine switches: underruns +{s['underruns'] - u0}  skips {s['skips']}  max {s['max_us']} us")
            if s["underruns"] - u0: fails.append(f"depth {depth} underruns")
        b.set_param("synth:depth", "2", overtake=False)

        print("\n-- idle sleep: 5 s of silence, then a note")
        time.sleep(5); s = status(b)
        asleep = s.get("parked") == 1
        note(b, 48, True); time.sleep(0.5); s2 = status(b); note(b, 48, False)
        print(f"  asleep after silence: {asleep}  (blocks skipped {s.get('parked_blocks')});  after a note: parked {s2.get('parked')} peak {s2['peak']}")
        if not asleep or s2.get("parked") != 0 or s2["peak"] == 0: fails.append("idle sleep")

        print("\n-- kill the engine inside Move")
        state = b.get_param("synth:state", overtake=False)
        pid = status(b)["pid"]; ssh(f"kill -9 {pid}")
        t, s = wait_ready(b, "synth")
        time.sleep(0.3)
        same = b.get_param("synth:state", overtake=False) == state
        note(b, 50, True); time.sleep(0.5); s = status(b); note(b, 50, False)
        print(f"  back in {t:.1f} s, respawns {s['respawns']}, state {'kept' if same else 'CHANGED'}, peak {s['peak']}")
        if not same or s["peak"] == 0: fails.append("respawn")

        print("\n-- Monomodule FX in fx1 behind it")
        b.set_param("fx1:module", "monomodule-fx", overtake=False)
        t, _ = wait_ready(b, "fx1"); print(f"  fx ready in {t:.1f} s")
        b.set_param("synth:machine", "SWAVE SAW", overtake=False)
        for m in FX:
            b.set_param("fx1:machine", m, overtake=False); time.sleep(0.2); status(b, "fx1")
            note(b, 45, True); time.sleep(0.6); s = status(b, "fx1"); note(b, 45, False); time.sleep(0.2)
            ok = s["peak"] > 0 and s["underruns"] == 0
            if not ok: fails.append("fx " + m)
            print(f"  {m:9s} peak {s['peak']:5d}  underruns {s['underruns']}  last {s['last_us']:4d} us  max {s['max_us']:4d} us  {'ok' if ok else 'FAIL'}")
        print(f"  engines running: {ssh('pgrep -c mnm-engine', check=False).decode().strip()}")

        print("\n-- restore the slot")
        b.set_param("fx1:module", prev_fx, overtake=False)
        b.set_param("synth:module", prev_synth, overtake=False)
        time.sleep(1.0)
        print(f"  synth={b.get_param('synth_module', overtake=False)!r} fx1={b.get_param('fx1_module', overtake=False)!r}  engines left: {ssh('pgrep -c mnm-engine', check=False).decode().strip() or '0'}")
    print("\nRESULT:", "PASS" if not fails else "FAIL " + ", ".join(fails))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
