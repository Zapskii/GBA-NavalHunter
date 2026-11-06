#!/usr/bin/env python3
"""Synthesises the sound effects from nothing and writes signed 8-bit PCM to src/sfx_data.c, which is
committed. Standard library only, no recordings, seeded: the same output byte for byte every run.
  python3 tools/gen_sfx.py [out.c]        default src/sfx_data.c

Timer 0 can only divide the clock by a whole number, so the rate here is the one the hardware really plays
at. The player stops a sample from the VBlank handler, N frames after it started, wherever in a frame that
was: between N-1 and N frames later. So each table entry is:
  - the sound, which starts and ends at exactly 0 on a short fade and fits inside N-1 frames: always all heard;
  - silence out to N whole frames, plus READ_AHEAD: the DMA runs ahead of what has played (a FIFO of 32
    bytes, and a request of 16 more), and it must read zeros, not the next sample, and leave 0 at the output.
"""
import math, random, sys

RELOAD = 1048                   # main.c reads this from sfx_reload: 65536 - it, every 1048 cycles
RATE = 16777216 / RELOAD        # 16,008.8 Hz
FRAME = 280896 / RELOAD         # 268.03 samples a frame
READ_AHEAD = 64

def seconds(s): return int(s * RATE)

def lowpass(x, hz):
    """One pole. hz: a number, or a function of the sample's index."""
    out, y = [], 0.0
    for i, v in enumerate(x):
        y += (1 - math.exp(-2 * math.pi * (hz(i) if callable(hz) else hz) / RATE)) * (v - y)
        out.append(y)
    return out

def highpass(x, hz):
    return [a - b for a, b in zip(x, lowpass(x, hz))]

def sweep(a, b, n):
    """From a down to b over n samples, evenly in pitch."""
    return lambda i: a * (b / a) ** (min(i, n) / n)

def noise(rng, n): return [rng.uniform(-1, 1) for _ in range(n)]

def tone(n, hz):
    """A sine whose pitch may change: hz is a function of the sample's index."""
    out, ph = [], 0.0
    for i in range(n):
        ph += 2 * math.pi * hz(i) / RATE
        out.append(math.sin(ph))
    return out

def finish(x, peak, fade_in, fade_out=400, drive=1.0):
    """Saturate gently (noise is all peaks, and this brings up the body), fade both ends to exactly 0, scale to a
    peak, round. The peak is what sets the balance between sounds."""
    top = max(abs(v) for v in x)
    x = [math.tanh(drive * v / top) for v in x]
    n = len(x)
    for i in range(fade_in): x[i] *= i / fade_in
    for i in range(fade_out): x[n - 1 - i] *= i / fade_out
    top = max(abs(v) for v in x)
    return [int(round(v * peak / top)) for v in x]

def splash(rng):
    """Filtered noise with a fast attack and a soft decay, then bubbles: short sines that rise."""
    n = seconds(0.75)
    wash = highpass(lowpass(noise(rng, n), sweep(3600, 800, n)), 280)
    x = [v * math.exp(-i / (0.11 * RATE)) for i, v in enumerate(wash)]
    for _ in range(11):
        at, m, hz = rng.randint(seconds(0.12), seconds(0.62)), seconds(rng.uniform(0.025, 0.06)), rng.uniform(420, 1050)
        level = 0.09 * rng.uniform(0.5, 1) * (1 - at / n)
        for i, v in enumerate(tone(m, lambda k: hz * (1 + 0.5 * k / m))):
            if at + i < n: x[at + i] += level * v * math.sin(math.pi * i / m)
    return finish(x, 64, 48, drive=1.8)

def explosion(rng):
    """Low-passed noise, a sharp attack, and a thump under it."""
    n = seconds(0.95)
    body = lowpass(noise(rng, n), sweep(3000, 240, seconds(0.5)))
    thump = tone(n, sweep(120, 55, seconds(0.3)))
    x = [(1.7 * b * math.exp(-i / (0.2 * RATE)) + 0.5 * t * math.exp(-i / (0.16 * RATE))) for i, (b, t) in enumerate(zip(body, thump))]
    return finish(highpass(x, 25), 112, 16, drive=2.5)

def boom(rng):
    """Longer and deeper: the filter and a low tone both sweep down, and a slow rumble carries the tail."""
    n = seconds(1.75)
    body = lowpass(noise(rng, n), sweep(1500, 110, seconds(0.9)))
    tail = lowpass(noise(rng, n), 170)
    wobble = lowpass(noise(rng, n), 7)
    w = max(abs(v) for v in wobble)
    low = tone(n, sweep(96, 38, seconds(1.1)))
    x = []
    for i in range(n):
        t = i / RATE
        x.append(2.0 * body[i] * math.exp(-t / 0.28)
                 + 0.6 * low[i] * math.exp(-t / 0.55)
                 + 3.0 * tail[i] * (0.65 + 0.35 * wobble[i] / w) * math.exp(-t / 0.62))
    return finish(highpass(x, 22), 120, 24, 1200, drive=3.0)

def ping(rng):
    """A clean decaying sine and two faint echoes. Alone on a silent menu, so well under the others."""
    n = seconds(0.42)
    hz = 1175
    x = [0.0] * n
    for delay, level in ((0, 1.0), (0.13, 0.27), (0.26, 0.08)):
        d = seconds(delay)
        for i in range(n - d):
            t = i / RATE
            x[d + i] += level * math.sin(2 * math.pi * hz * t) * math.exp(-t / 0.07) * min(1, t / 0.003)
    return finish(x, 48, 1)

SOUNDS = (("splash", splash), ("explosion", explosion), ("boom", boom), ("ping", ping))     # main.c's enum order

def frames_for(n):
    """The whole frames a sound of n samples needs: its n fit inside the N-1 that are certain to play."""
    return math.ceil(n / FRAME) + 1

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "src/sfx_data.c"
    rng = random.Random(0x5EA)
    out = ["/* Generated by tools/gen_sfx.py. Do not edit. Signed 8-bit PCM, played from ROM, so word-aligned",
           "   for 32-bit DMA. Each ends in silence: see the top of the script. */",
           "#include <stdint.h>", "",
           "typedef struct { const int8_t *data; uint32_t len; uint16_t frames; } Sfx;", "",
           f"const uint16_t sfx_reload = {RELOAD};     /* {RATE:.1f} Hz */", ""]
    table = []
    total = 0
    for name, make in SOUNDS:
        s = make(rng)
        assert s[0] == 0 and s[-1] == 0 and max(map(abs, s)) <= 127, name
        frames = frames_for(len(s))
        length = math.ceil(frames * FRAME) + READ_AHEAD
        length += -length % 4
        data = s + [0] * (length - len(s))
        rms = math.sqrt(sum(v * v for v in s) / len(s))
        print(f"{name:10s} {len(s) / RATE:5.2f} s, {len(s):6d} samples + silence = {length:6d} bytes, {frames:3d} frames, peak {max(map(abs, s)):3d}, rms {rms:5.1f}")
        total += length
        out.append(f"const int8_t sfx_{name}[{length}] __attribute__((aligned(4))) = {{")
        out += [",".join(map(str, data[i:i + 32])) + "," for i in range(0, length, 32)]
        out += ["};", ""]
        table.append(f"    {{ sfx_{name}, {length}, {frames} }},")
    assert total < 256 * 1024
    print(f"total {total} bytes, of 262144")
    out += ["const Sfx sfx_table[4] = {"] + table + ["};", ""]
    with open(path, "w", newline="\n") as f: f.write("\n".join(out))

main()
