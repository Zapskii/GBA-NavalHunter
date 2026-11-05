#!/usr/bin/env python3
"""Drives the real ROM in libmgba (tools/emu.c). What it pins is what only the ROM can
show. Screenshots go to shots/. Exit 1 on any FAIL."""
import os, subprocess, sys
from PIL import Image

DISPCNT, VRAM = 0x04000000, 0x06000000
ST_BOOT, ST_READY = range(2)
C_SEA, C_INK, C_RED, C_YEL = range(4)      # main.c's palette indexes

fails = 0
def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    if not ok: fails += 1

class Emu:
    def __init__(self, rom):
        self.p = subprocess.Popen(["./emu", rom], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.sym = {}
        for l in open("navalhunter.sym"):
            f = l.split()
            if len(f) == 3: self.sym[f[2]] = int(f[0], 16)
    def cmd(self, s):
        self.p.stdin.write(s + "\n"); self.p.stdin.flush()
        return self.p.stdout.readline().strip()
    def run(self, n=1, keys=0): self.cmd(f"run {n} {keys}")
    def peek(self, a, n=1): return bytes.fromhex(self.cmd(f"peek {a:x} {n}"))
    def poke(self, a, b): self.cmd(f"poke {a:x} {b.hex()}")
    def u8(self, name): return self.peek(self.sym[name])[0]
    def u32(self, name): return int.from_bytes(self.peek(self.sym[name], 4), "little")
    def set8(self, name, v): self.poke(self.sym[name], bytes([v]))
    def set16(self, name, v): self.poke(self.sym[name], v.to_bytes(2, "little"))
    def set32(self, name, v): self.poke(self.sym[name], v.to_bytes(4, "little"))
    def reg(self, a): return int.from_bytes(self.peek(a, 2), "little")
    def tap(self, key): self.run(2, key); self.run(2)
    def px(self, x, y): return self.peek(VRAM + y * 240 + x)[0]
    def shot(self, name):
        os.makedirs("shots", exist_ok=True)
        self.cmd(f"shot shots/{name}.ppm")
        Image.open(f"shots/{name}.ppm").resize((480, 320), Image.NEAREST).save(f"shots/{name}.png")
        os.remove(f"shots/{name}.ppm")
    def until(self, pred, limit=3000, keys=0):
        for _ in range(limit):
            if pred(): return True
            self.run(1, keys)
        return False

def main():
    e = Emu("navalhunter.gba")
    e.run(10)
    check(e.u8("state") == ST_READY, f"booted: state {e.u8('state')}")

    d = e.reg(DISPCNT)
    check(d & 7 == 4, f"Mode 4 in REG_DISPCNT ({d:#06x})")
    check(d & 0x10 == 0, "showing page 0")
    check(d & 0x400 and d & 0x1000, "BG2 and sprites on")
    check(d & 0x80 == 0, "not force-blanked")

    # --- pixels in one halfword must not clobber each other, whichever is drawn first
    for y in (90, 92):
        check(e.px(100, y) == C_RED, f"even-x pixel (100,{y}) correct in VRAM: {e.px(100, y)}")
        check(e.px(101, y) == C_YEL, f"odd-x pixel (101,{y}) correct in VRAM: {e.px(101, y)}")
        check(e.px(99, y) == C_SEA and e.px(102, y) == C_SEA, f"neighbours of the pair untouched at y={y}")
    for y in (94, 95):
        row = [e.px(x, y) for x in range(104, 110)]
        check(row == [C_SEA, C_RED, C_RED, C_RED, C_RED, C_SEA], f"rect from odd x to odd end, y={y}: {row}")

    # --- gfx_text: the N of the title, at (84,40)
    n = ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"]
    got = ["".join("#" if e.px(84 + x, 40 + y) == C_INK else "." for x in range(5)) for y in range(7)]
    check(got == n, f"text draws the font's N: {got}")

    # --- no dropped frame, and the frames really ran
    check(e.u32("frame_drops") == 0, f"no frame dropped at boot ({e.u32('frame_drops')})")
    f0 = e.u32("frames")
    e.run(300)
    check(e.u32("frames") - f0 == 300, f"VBlank IRQ ran for all 300 frames ({e.u32('frames') - f0})")
    check(e.u32("frame_drops") == 0, f"frame_drops still 0 after 300 frames ({e.u32('frame_drops')})")

    # --- and the screen shows what VRAM holds
    e.shot("1-boot")
    im = Image.open("shots/1-boot.png").convert("RGB")
    r, g, b = im.getpixel((100 * 2, 90 * 2))
    check(r > 200 and g < 60, f"even-x pixel red on screen: {(r, g, b)}")
    r, g, b = im.getpixel((101 * 2, 90 * 2))
    check(r > 200 and g > 200 and b < 60, f"odd-x pixel yellow on screen: {(r, g, b)}")

    e.p.stdin.close(); e.p.wait()
    print("FAILED" if fails else "all good", f"({fails} failures)")
    return 1 if fails else 0

sys.exit(main())
