#!/usr/bin/env python3
"""Drive and watch a Move running STOCK Schwung from the dev machine: read the OLED, press pads and
buttons, turn the jog, read and set module parameters.

Nothing is installed or changed on the device:
  * the screen is read from /dev/shm/schwung-display-live, the frame the stock shim already copies
    out for the Remote UI's screen viewer whenever the display_mirror flag (byte 33 of
    /dev/shm/schwung-control) is on. We switch it on if it is off, and back off when done.
  * gestures and params go through upstream's own test bus: schwung-testd (shipped in the stock
    install's bin/) and its Python client, schwung-current/tools/pytest-schwung. The daemon only
    reaches chain slot 0, i.e. track 1.

CLI:
  move.py screen                  print the screen
  move.py tap jog_click|shift|menu|back [hold_frames]
  move.py jog N                   N detents (negative = left)
  move.py pad NOTE [ms]           press a pad (notes 68-99) and release after ms
  move.py get KEY                 e.g. synth:status   (slot 0 = track 1)
  move.py set KEY VALUE           e.g. synth:machine "SID 6581"
Each gesture prints the screen afterwards and how many bytes of the frame changed.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

HOST = os.environ.get("MOVE_HOST", "move.local")
USER = "ableton"
PORT = 47777
TESTD = "/data/UserData/schwung/bin/schwung-testd"
LIVE = "/dev/shm/schwung-display-live"
CONTROL = "/dev/shm/schwung-control"
MIRROR_OFFSET = 33   # shadow_control_t.display_mirror (schwung src/host/shadow_constants.h), checked by offsetof
CONTROL_SOCKET = f"/tmp/move-devicetest-{os.getuid()}.sock"   # on the Mac
SCRATCH = "/data/UserData/mnm-scratch"   # on the Move: never /tmp (a stock Move's root FS is ~463 MB and full)
DAVEBOX_LOCK = "/dev/shm/.dbxhost-session.lock"

_client_src = Path(os.environ.get("SCHWUNG_CURRENT", Path(__file__).resolve().parents[3] / "schwung-current")) / "tools/pytest-schwung/src"
sys.path.insert(0, str(_client_src))


def ssh(cmd: str, *, input: bytes | None = None, check: bool = True) -> bytes:
    """One command over a shared connection (ControlMaster), so each call costs a round trip, not a handshake."""
    args = ["ssh", "-o", "BatchMode=yes", "-o", "ControlMaster=auto", "-o", f"ControlPath={CONTROL_SOCKET}",
            "-o", "ControlPersist=120", "-o", "LogLevel=ERROR", f"{USER}@{HOST}", cmd]
    r = subprocess.run(args, input=input, capture_output=True)
    if check and r.returncode != 0:
        raise RuntimeError(f"ssh {cmd!r}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout


@dataclass
class Frame:
    """128x64 1-bit OLED frame. PAGE-ordered: byte = (y // 8) * 128 + x, bit = y % 8, LSB topmost."""
    data: bytes

    def pixel(self, x: int, y: int) -> bool:
        return bool((self.data[(y // 8) * 128 + x] >> (y % 8)) & 1)

    def is_blank(self) -> bool:
        return not any(self.data)

    def changed(self, other: "Frame") -> int:
        return sum(1 for a, b in zip(self.data, other.data) if a != b)

    def to_ascii(self) -> str:
        """Two pixel rows per text line, half blocks: a readable 32-line picture of the screen."""
        lines = []
        for y in range(0, 64, 2):
            row = []
            for x in range(128):
                a, b = self.pixel(x, y), self.pixel(x, y + 1)
                row.append("█" if a and b else "▀" if a else "▄" if b else " ")
            lines.append("".join(row).rstrip())
        return "\n".join(lines)


class Screen:
    def __init__(self):
        self._we_enabled = False

    def __enter__(self):
        if ssh(f"od -An -tu1 -j{MIRROR_OFFSET} -N1 {CONTROL}").strip() != b"1":
            self._set_mirror(1)
            self._we_enabled = True
            time.sleep(0.1)   # a few frames for the first copy
        return self

    def __exit__(self, *_):
        if self._we_enabled:
            self._set_mirror(0)

    @staticmethod
    def _set_mirror(v: int):
        ssh(f"printf '\\{v:03o}' | dd of={CONTROL} bs=1 seek={MIRROR_OFFSET} count=1 conv=notrunc 2>/dev/null")

    def snapshot(self) -> Frame:
        d = ssh(f"cat {LIVE}")
        if len(d) != 1024:
            raise RuntimeError(f"live display is {len(d)} bytes, expected 1024")
        return Frame(d)


def davebox_session_live() -> bool:
    """dAVEBOx SA owns the whole surface while its session runs; the lock holds its PID."""
    out = ssh(f"p=$(cat {DAVEBOX_LOCK} 2>/dev/null); [ -n \"$p\" ] && kill -0 $p 2>/dev/null && echo live", check=False)
    return b"live" in out


class Device:
    """Screen + upstream's test bus. Starts schwung-testd if it is not running and forwards its port.
    Refuses to start while a dAVEBOx session is live on the same Move (another session's work)."""

    def __enter__(self):
        if davebox_session_live() and not os.environ.get("MNM_IGNORE_DAVEBOX"):
            raise SystemExit("a dAVEBOx session is live on this Move: ask Josh before driving it")
        from schwung_bus.client import SchwungBus   # upstream's client
        self.screen = Screen().__enter__()
        if not ssh("pgrep -x schwung-testd", check=False).strip():
            ssh(f"mkdir -p {SCRATCH}; nohup setsid {TESTD} > {SCRATCH}/schwung-testd.log 2>&1 < /dev/null &")
            time.sleep(0.5)
        subprocess.run(["ssh", "-o", f"ControlPath={CONTROL_SOCKET}", "-O", "forward", "-L",
                        f"{PORT}:127.0.0.1:{PORT}", f"{USER}@{HOST}"], capture_output=True)
        self.bus = SchwungBus()
        self.bus.connect()
        return self

    def __exit__(self, *exc):
        self.bus.close()
        self.screen.__exit__(*exc)

    def settle(self, frames: int = 20) -> Frame:
        self.bus.wait_frame(frames)
        return self.screen.snapshot()

    def jog(self, detents: int):
        for _ in range(abs(detents)):
            self.bus.inject_midi(bytes([0x0B, 0xB0, 14, 1 if detents > 0 else 127]))
            self.bus.wait_frame(4)   # detents in one burst are queued and land late

    def pad(self, note: int, ms: int = 300):
        self.bus.press_pad(note)
        time.sleep(ms / 1000)
        self.bus.release_pad(note)


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd, rest = argv[0], argv[1:]
    if cmd == "screen":
        with Screen() as s:
            print(s.snapshot().to_ascii())
        return 0
    with Device() as d:
        before = d.screen.snapshot()
        if cmd == "tap":
            d.bus.tap(rest[0], int(rest[1]) if len(rest) > 1 else 2)
        elif cmd == "jog":
            d.jog(int(rest[0]))
        elif cmd == "pad":
            d.pad(int(rest[0]), int(rest[1]) if len(rest) > 1 else 300)
        elif cmd == "get":
            print(d.bus.get_param(rest[0], overtake=False))
            return 0
        elif cmd == "set":
            d.bus.set_param(rest[0], rest[1], overtake=False)
        else:
            print(f"unknown command {cmd!r}\n{__doc__}")
            return 2
        after = d.settle()
        print(after.to_ascii())
        print(f"-- {after.changed(before)} of 1024 bytes changed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
