#!/usr/bin/env python3
"""Writes src/art.c, src/art.h and the preview tools/art.png. Every pixel is drawn here, from
nothing: no source image is read. Run by hand (python3 tools/gen_art.py); the output is committed,
so a plain `make` needs no Python.

PALETTE PLAN (BG palette, 256 entries)
    0-15    SEA. A periodic ramp: Phase 6 rotates these 16 and every water pixel moves. Water is
            drawn only in these, on the boards and on the full screens, and nothing else uses them.
            Entry 0 is also the backdrop. No ink, hull or chrome pixel may sit here.
    16-63   SHARED: boards, ships, markers, font, UI. Loaded once at boot and never changed, so going
            back from a full screen has nothing to restore in 0-63.
    64-255  ONE FULL SCREEN at a time: title, win or lose loads its own over this range while the
            screen is black. Nothing else draws with them.
A full screen may also use 0-63: its water is the same sea, and a colour already in SHARED costs
no slot.

OBJ PALETTE (16 entries, 4bpp sprites): see OBJ_PAL below. Sprites never use the BG palette, so nothing
on a sprite cycles with the sea.
"""
import math, os
from PIL import Image, ImageDraw

W, H = 240, 160
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ART_FIRST, ART_N = 64, 192

# ---------------------------------------------------------------- the palettes
def sea_colour(k):
    """Phase k of 16: long deep troughs and a short bright crest, so a calm sea with thin moving lines."""
    a = (0.5 + 0.5 * math.cos(2 * math.pi * k / 16)) ** 5
    b = 0.5 + 0.5 * math.cos(2 * math.pi * (k - 8) / 16)
    deep, mid, crest = (16, 56, 112), (24, 72, 136), (88, 160, 208)
    c = [deep[i] + (mid[i] - deep[i]) * b * 0.8 for i in range(3)]
    return tuple(int(c[i] + (crest[i] - c[i]) * a) for i in range(3))
SEA = [sea_colour(k) for k in range(16)]

SHARED = [  # name, colour, from index 16
    ("C_BG", (8, 20, 40)), ("C_INK", (248, 248, 248)), ("C_GRID", (40, 88, 128)), ("C_YEL", (248, 232, 64)),
    ("C_DIM", (136, 168, 200)), ("C_RED", (240, 56, 40)), ("C_SHADOW", (0, 8, 24)),
    # hull, lit to dark
    ("C_H0", (240, 244, 240)), ("C_H1", (192, 204, 208)), ("C_H2", (144, 160, 172)), ("C_H3", (96, 112, 128)),
    ("C_H4", (56, 68, 88)), ("C_HO", (16, 24, 40)), ("C_WIN", (112, 216, 240)),
    # fire, hot to cold
    ("C_F0", (255, 255, 216)), ("C_F1", (255, 216, 48)), ("C_F2", (248, 128, 24)), ("C_F3", (208, 40, 24)),
    # burnt hull
    ("C_S1", (112, 88, 80)), ("C_S2", (72, 56, 56)), ("C_S3", (40, 32, 40)), ("C_S4", (16, 12, 20)),
    # the refused preview
    ("C_B1", (255, 168, 136)), ("C_B2", (232, 96, 72)), ("C_B3", (160, 48, 40)),
    # water marks
    ("C_MISS", (232, 248, 255)), ("C_KNOWN", (152, 208, 232)),
]
assert 16 + len(SHARED) <= ART_FIRST
SHARED_COL = {n: c for n, c in SHARED}
SHARED_IDX = {n: 16 + i for i, (n, _) in enumerate(SHARED)}

def snap(c): return tuple(v & 0xF8 for v in c[:3])
def rgb15(c): return (c[0] >> 3) | (c[1] >> 3) << 5 | (c[2] >> 3) << 10
assert len({snap(c) for _, c in SHARED}) == len(SHARED), "two shared colours are the same in 15 bits"

# ---------------------------------------------------------------- the cell art
# Inner cell art, bow to the right. '.' is water. A tile adds the grid line on its top and left.
LEGEND = {"o": "C_HO", "L": "C_H1", "M": "C_H2", "D": "C_H3", "T": "C_H4", "H": "C_HO", "W": "C_WIN", "E": "C_H0"}
HULL_BIG = {
    "stern": """
...........
...oooooooo
..oEEEEEEEE
.oLLLLLLLLL
oLMMMMTTTMM
oMHHHHTLTMM
oMMMMMTTTMM
.oMMMMMMMMM
..oDDDDDDDD
...oooooooo
...........""",
    "mid": """
...........
ooooooooooo
EEEEEEEEEEE
LLLLLLLLLLL
MMTTTTTTTMM
MMTWWWWWTMM
MMTTTTTTTMM
MMMMMMMMMMM
DDDDDDDDDDD
ooooooooooo
...........""",
    "bow": """
...........
oooooo.....
EEEEEEoo...
LLLLLLLLoo.
MTTTMMMMMLo
MTLTHHHHHMo
MTTTMMMMMDo
MMMMMMMMoo.
DDDDDDoo...
oooooo.....
...........""",
    "boat": """
...........
...........
..oooooo...
.oEEEEEEoo.
oLLLLLLLLLo
oMMTWWTMMMo
oMMMMMMMMDo
.oDDDDDDoo.
..oooooo...
...........
...........""",
}
JOINT_BIG = ".oELMMMMDo."       # one slice of "mid", for where a hull runs over the grid line
HULL_MINI = {
    "stern": """
.......
..EEEEE
.LLLLLL
.MMTTMM
.MMMMMM
..DDDDD
.......""",
    "mid": """
.......
EEEEEEE
LLLLLLL
MTTWTTM
MMMMMMM
DDDDDDD
.......""",
    "bow": """
.......
EEEEE..
LLLLLEE
MTMMMML
MMMMMDD
DDDDD..
.......""",
    "boat": """
.......
.......
.EEEE..
LLLLLLE
MTTWMMM
.DDDD..
.......""",
}
JOINT_MINI = ".ELMMD."

# Marks. Each is a different shape, so none relies on colour: a ring is a miss, a spiked cross a
# hit, a thin scatter of dots is known water.
MARK_BIG = {
    "miss": """
...........
....www....
..wwwwwww..
..ww...ww..
.ww.....ww.
.ww.....ww.
.ww.....ww.
..ww...ww..
..wwwwwww..
....www....
...........""",
    "hit": """
...........
.r.......r.
.rOr...rOr.
..OYr.rYO..
...YYOYY...
....OWO....
...YYOYY...
..OYr.rYO..
.rOr...rOr.
.r.......r.
...........""",
    "known": """
...........
.k...k...k.
...........
...k...k...
...........
.k...k...k.
...........
...k...k...
...........
.k...k...k.
...........""",
    "wreck": """
...........
.R.......R.
..RR...RR..
...RR.RR...
....RRR....
.....R.....
....RRR....
...RR.RR...
..RR...RR..
.R.......R.
...........""",
}
MARK_MINI = {
    "miss": """
.......
..www..
.w...w.
.w...w.
.w...w.
..www..
.......""",
    "hit": """
r.....r
.OY.YO.
.YYOYY.
..OWO..
.YYOYY.
.OY.YO.
r.....r""",
    "known": """
.......
.k...k.
.......
...k...
.......
.k...k.
.......""",
    "wreck": """
R.....R
.R...R.
..R.R..
...R...
..R.R..
.R...R.
R.....R""",
}
MARK_COL = {"w": "C_MISS", "r": "C_F3", "O": "C_F2", "Y": "C_F1", "W": "C_F0", "k": "C_KNOWN", "R": "C_RED", "x": "C_HO"}

def rows(s): return s.strip("\n").split("\n")

def water_tile(s):
    """s x s of sea phases. Period s across, so the crests run on from cell to cell over the grid."""
    pitch = 6 if s == 12 else 4
    out = []
    for y in range(s):
        out.append([int(16 * (y / pitch + 0.14 * math.sin(2 * math.pi * x / s))) % 16 for x in range(s)])
    return out

def recolour(name, scheme):
    if scheme == "plain": return SHARED_IDX[name]
    m = {"C_H0": "C_S1", "C_H1": "C_S1", "C_H2": "C_S2", "C_H3": "C_S3", "C_H4": "C_S4", "C_HO": "C_S4", "C_WIN": "C_S4"} \
        if scheme == "sunk" else \
        {"C_H0": "C_B1", "C_H1": "C_B1", "C_H2": "C_B2", "C_H3": "C_B3", "C_H4": "C_B3", "C_HO": "C_HO", "C_WIN": "C_B3"}
    return SHARED_IDX[m[name]]

SEGS = ["H_STERN", "H_MID", "H_BOW", "V_BOW", "V_MID", "V_STERN", "BOAT"]

def rot_ccw(g):
    """Bow right to bow up."""
    n = len(g)
    return [[g[x][n - 1 - y] for x in range(n)] for y in range(n)]

def hull_inner(hull, part, scheme):
    return [[-1 if c == "." else recolour(LEGEND[c], scheme) for c in r] for r in rows(hull[part])]

def segment_tile(s, hull, joint, seg, scheme):
    """s x s, with the grid line on top and left, except where the hull runs over it to join the
    cell before: left in H, top in V. So a ship reads as one hull, not a row of boxes."""
    part = {"H_STERN": "stern", "H_MID": "mid", "H_BOW": "bow", "V_BOW": "bow", "V_MID": "mid", "V_STERN": "stern", "BOAT": "boat"}[seg]
    inner = hull_inner(hull, part, scheme)
    jn = [-1 if c == "." else recolour(LEGEND[c], scheme) for c in joint]
    water, grid = water_tile(s), SHARED_IDX["C_GRID"]
    t = [[grid] * s for _ in range(s)]
    if seg[0] == "V":
        inner = rot_ccw(inner)
    for y in range(s - 1):
        for x in range(s - 1):
            v = inner[y][x]
            t[y + 1][x + 1] = water[y + 1][x + 1] if v < 0 else v
    if seg in ("H_MID", "H_BOW"):
        for y in range(s - 1):
            if jn[y] >= 0: t[y + 1][0] = jn[y]
    if seg in ("V_MID", "V_STERN"):
        for x in range(s - 1):
            if jn[x] >= 0: t[0][x + 1] = jn[x]
    return t

def overlay(t, mark):
    for y, r in enumerate(rows(mark)):
        for x, c in enumerate(r):
            if c != ".": t[y + 1][x + 1] = SHARED_IDX[MARK_COL[c]]
    return t

def water_cell(s):
    t, grid = water_tile(s), SHARED_IDX["C_GRID"]
    for k in range(s): t[0][k] = t[k][0] = grid
    return t

TILES = ["T_WATER", "T_MISS", "T_HIT", "T_KNOWN"]
T = {n: i for i, n in enumerate(TILES)}
T.update(T_HULL=4, T_HULLHIT=11, T_SUNK=18, T_BAD=25, T_COUNT=32)

def tile_set(s, hull, joint, marks):
    out = [water_cell(s), overlay(water_cell(s), marks["miss"]), overlay(water_cell(s), marks["hit"]), overlay(water_cell(s), marks["known"])]
    for scheme, mark in (("plain", None), ("plain", marks["hit"]), ("sunk", marks["wreck"]), ("bad", marks["wreck"].replace("R", "x"))):
        for seg in SEGS:
            t = segment_tile(s, hull, joint, seg, scheme)
            out.append(overlay(t, mark) if mark else t)
    assert len(out) == T["T_COUNT"]
    return out

# ---------------------------------------------------------------- the fleet panel icons
ICON_W, ICON_H = 28, 7
def icon(n, sunk):
    """A hull n blocks long, stern left, on the panel's background. Alive: a solid square on each
    block. Sunk: an X on each block, on a dark hull. Count the blocks to read the length."""
    bg, o = SHARED_IDX["C_BG"], SHARED_IDX["C_HO"]
    hull = SHARED_IDX["C_S2" if sunk else "C_H3"]
    top = SHARED_IDX["C_S1" if sunk else "C_H1"]
    pip = SHARED_IDX["C_RED" if sunk else "C_H0"]
    g = [[bg] * ICON_W for _ in range(ICON_H)]
    w = 5 * n + 1
    for y in range(1, 6):
        for x in range(w):
            g[y][x] = top if y == 1 else hull
    g[1][0] = g[5][0] = bg                     # a rounded stern
    for y, trim in ((1, 2), (2, 1), (4, 1), (5, 2)):    # and a pointed bow
        for x in range(w - trim, w): g[y][x] = bg
    g[3][w] = hull
    for k in range(n):
        x0 = 5 * k + 1
        if k: 
            for y in range(1, 6): g[y][x0 - 1] = o
        for d in range(3):
            if sunk:
                g[2 + d][x0 + d] = pip
                g[2 + d][x0 + 2 - d] = pip
            else:
                for e in range(3): g[2 + d][x0 + e] = pip
    return g

# ---------------------------------------------------------------- the sprites
# Index 0 is see-through. 1 and 2 are the aiming cursor and the enemy-shot marker, which main.c draws.
OBJ_PAL = [(0, 0, 0), (255, 255, 32), (255, 255, 255),
           (32, 36, 56), (96, 112, 128), (192, 204, 208),                   # 3-5 the shell, dark to light
           (255, 255, 216), (255, 216, 48), (248, 128, 24), (208, 40, 24),  # 6-9 fire, hot to cold
           (168, 168, 176), (112, 112, 124), (64, 64, 76),                  # 10-12 smoke, light to dark
           (232, 248, 255), (152, 208, 232), (72, 136, 192)]                # 13-15 spray, light to dark
SHELL_D, SHELL_M, SHELL_L = 3, 4, 5
HOT, YEL, ORG, RED = 6, 7, 8, 9
SMK_L, SMK_M, SMK_D = 10, 11, 12
FOAM, PALE, BLUE = 13, 14, 15

def polar(n):
    """Every pixel of an n x n sprite, as x, y, distance from the centre and angle."""
    c = (n - 1) / 2
    for y in range(n):
        for x in range(n):
            yield x, y, math.hypot(x - c, y - c), math.atan2(y - c, x - c)

def obj_shell():
    """A round shot seen from above, lit from the top left. It has room to scale up inside 16 x 16."""
    g = [[0] * 16 for _ in range(16)]
    for x, y, d, _ in polar(16):
        if d <= 4.3: g[y][x] = SHELL_D if d > 3.3 else SHELL_M
        if math.hypot(x - 6, y - 6) <= 1.3: g[y][x] = SHELL_L
    return g

def obj_splash(k):
    """A ring of foam spreading and breaking up, round a column of water that falls back."""
    ring, col = (2.5, 4.5, 6, 7)[k], (1.5, 2.5, 1.5, 0)[k]
    gaps = (99, 12, 8, 6)[k]        # how many pieces the ring breaks into
    c = (FOAM, FOAM, PALE, BLUE)[k]
    g = [[0] * 16 for _ in range(16)]
    for x, y, d, a in polar(16):
        if abs(d - ring) <= 0.7 and (k == 0 or math.cos(a * gaps) > -0.3): g[y][x] = c
        if d <= col: g[y][x] = FOAM if d <= col - 1 else PALE
    return g

def obj_blast(k):
    """A spiked fireball swelling, then burning out into smoke with holes in it."""
    r = (3, 5.5, 7, 7.5, 7.5)[k]
    g = [[0] * 16 for _ in range(16)]
    for x, y, d, a in polar(16):
        edge = r * (0.8 + 0.2 * math.cos(7 * a + k * 2))
        if d > edge: continue
        t = d / edge
        if k < 3: c = HOT if t < 0.35 else YEL if t < 0.6 else ORG if t < 0.85 else RED
        elif k == 3: c = 0 if t < 0.3 and (x + y) & 1 else RED if t < 0.5 else ORG if t < 0.7 else SMK_D
        else: c = (SMK_D if t > 0.6 else SMK_M) if (x + y) & 1 and t > 0.35 else 0
        g[y][x] = c
    return g

def obj_smoke(k):
    """Four looping frames, 8 x 8: a lick of flame low down, and puffs rising off it. Small, with the
    cell's mark still showing round it."""
    g = [[0] * 8 for _ in range(8)]
    def blob(cx, cy, r, c):
        for y in range(8):
            for x in range(8):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r: g[y][x] = c
    for off, c, r in ((0, SMK_L, 1.5), (3, SMK_M, 2.0), (6, SMK_D, 1.5)):
        h = (k * 2 + off) % 8                   # height over the flame; a whole loop rises 8
        if h < 6: blob(3 + (1 if h in (2, 3) else 0) - (1 if h > 4 else 0), 5 - h, r if h > 1 else 1.0, c)
    g[6][3] = g[7][3] = ORG
    g[6][3 + (k & 1)] = YEL
    g[5 + (k >> 1 & 1)][3 - (k & 1)] = RED
    return g

def obj_tiles(g):
    """A sprite to 4bpp tiles, in 1D order: left to right, then down."""
    out = bytearray()
    n = len(g)
    for ty in range(0, n, 8):
        for tx in range(0, n, 8):
            for y in range(8):
                for x in range(0, 8, 2):
                    out.append(g[ty + y][tx + x] | g[ty + y][tx + x + 1] << 4)
    return bytes(out)

OBJ_FRAMES = [("OBJ_SHELL", [obj_shell()]), ("OBJ_SPLASH", [obj_splash(k) for k in range(4)]),
              ("OBJ_BLAST", [obj_blast(k) for k in range(5)]), ("OBJ_SMOKE", [obj_smoke(k) for k in range(4)])]

# ---------------------------------------------------------------- pixel canvas helpers
SEA_KEY = lambda k: (1, 2, k)                  # not a multiple of 8, so no painted colour can equal it
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

def mix(a, b, t): return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

def sky(px, stops, y0, y1):
    """Vertical bands of the stops, ordered-dithered where they meet."""
    n = len(stops) - 1
    for y in range(y0, y1):
        t = (y - y0) / (y1 - y0 - 1) * n
        i = min(int(t), n - 1)
        f = t - i
        # hold each band flat and dither only the last part, so the bands read as bands
        f = max(0.0, (f - 0.55) / 0.45)
        for x in range(W):
            px[x, y] = stops[i + 1] if f * 16 > BAYER[y & 3][x & 3] else stops[i]

def disc(px, cx, cy, r, c):
    for y in range(cy - r, cy + r + 1):
        for x in range(cx - r, cx + r + 1):
            if 0 <= x < W and 0 <= y < H and (x - cx) ** 2 + (y - cy) ** 2 <= r * r + r * 0.6: px[x, y] = c

def halo(px, cx, cy, r0, r1, c):
    """A dithered ring: c fades out from r0 to r1."""
    for y in range(max(0, cy - r1), min(H, cy + r1 + 1)):
        for x in range(max(0, cx - r1), min(W, cx + r1 + 1)):
            d = math.hypot(x - cx, y - cy)
            if r0 <= d <= r1 and (1 - (d - r0) / (r1 - r0)) * 16 > BAYER[y & 3][x & 3] + 0.5: px[x, y] = c

def sun(px, cx, cy, r, ramp):
    halo(px, cx, cy, r, r + 12, ramp[0])
    disc(px, cx, cy, r + 2, ramp[1])
    disc(px, cx, cy, r, ramp[2])
    disc(px, cx - r // 4, cy - r // 4, r * 3 // 5, ramp[3])

def cloud(px, x0, y0, w, h, light, mid, dark):
    """A flat-bottomed heap of bumps, lit from above, with a hard shaded base."""
    bumps = []
    n = max(3, w // 12)
    for i in range(n):
        t = (i + 0.5) / n
        bumps.append((x0 + t * w, y0 + h - h * 0.35, (0.35 + 0.65 * math.sin(math.pi * t)) * h * 0.62 + (i * 7 % 3)))
    def inside(x, y):
        return y <= y0 + h and any((x - bx) ** 2 + ((y - by) * 1.15) ** 2 <= br ** 2 for bx, by, br in bumps)
    for y in range(max(0, int(y0 - h)), min(H, y0 + h + 1)):
        for x in range(max(0, x0 - 8), min(W, x0 + w + 8)):
            if not inside(x, y): continue
            if y >= y0 + h - 1 or (y >= y0 + h - 3 and (x + y) & 1 == 0): c = dark
            elif not inside(x, y - 2) or not inside(x - 1, y - 1): c = light
            else: c = mid if y > y0 + h * 0.45 or (x + y) % 5 else light
            px[x, y] = c

def sea(px, y0, y1=H):
    """The cycling sea. Crest lines bunch up at the horizon and spread towards the viewer."""
    phi = 0.0
    for y in range(y0, y1):
        t = (y - y0) / (H - y0)
        pitch = 2.2 + 7 * t
        px_w = 22 + 70 * t
        for x in range(W):
            v = phi + 0.22 * math.sin(2 * math.pi * x / px_w + y * 1.7) + 0.10 * math.sin(2 * math.pi * x / (px_w * 0.37) + y)
            px[x, y] = SEA_KEY(int(16 * v) % 16)
        phi += 1 / pitch

def swell(px, y, amp, period, phase, body, crest, y1=H, foam=None):
    """A static band of nearer water in front of the cycling sea, with a lit edge and foam."""
    for x in range(W):
        h = y + amp * math.sin(2 * math.pi * x / period + phase) + amp * 0.4 * math.sin(2 * math.pi * x / (period * 0.43) + phase * 2)
        h = int(h)
        for yy in range(max(h, 0), y1):
            px[x, yy] = crest if yy == h else body
        if foam and math.sin(2 * math.pi * x / period + phase) > 0.72 and h - 1 >= 0:
            px[x, h] = foam
            if x % 3 == 0: px[x, h - 1] = foam

# ---------------------------------------------------------------- letters
GLYPH = {
    "A": [0x04, 0x0A, 0x0A, 0x11, 0x1F, 0x11, 0x11], "E": [0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F],
    "H": [0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11], "I": [0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E],
    "L": [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F], "N": [0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11],
    "O": [0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E], "R": [0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11],
    "S": [0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E], "T": [0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04],
    "U": [0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E], "V": [0x11, 0x11, 0x11, 0x0A, 0x0A, 0x0A, 0x04],
    "W": [0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11], "Y": [0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04],
    " ": [0] * 7,
}
def logo(px, text, y, scale, ramp, edge, shadow):
    """Heavy block capitals: thick strokes, a banded metal fill, a lit top edge, a hard outline."""
    adv = 6 * scale
    x0 = (W - (adv * len(text) - scale)) // 2
    on = set()
    for k, ch in enumerate(text):
        for r, bits in enumerate(GLYPH[ch]):
            for c in range(5):
                if bits & (16 >> c):
                    for dy in range(-1, scale + 1):
                        for dx in range(-1, scale + 1):
                            on.add((x0 + k * adv + c * scale + dx, y + r * scale + dy))
    # knock the outer corner off each glyph block for a chamfered look
    on = {p for p in on if sum((p[0] + a, p[1] + b) in on for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1))) >= 2}
    around = lambda p, d: [(p[0] + a, p[1] + b) for a in range(-d, d + 1) for b in range(-d, d + 1)]
    for p in {q for p in on for q in around(p, 2)}:
        s = (p[0] + 2, p[1] + 2)
        if 0 <= s[0] < W and 0 <= s[1] < H: px[s] = shadow
    for p in {q for p in on for q in around(p, 1)}:
        if 0 <= p[0] < W and 0 <= p[1] < H: px[p] = edge
    h = 7 * scale + 2
    for p in on:
        t = (p[1] - y + 1) / h
        c = ramp[min(int(t * len(ramp)), len(ramp) - 1)]
        if (p[0], p[1] - 1) not in on: c = ramp[0]
        elif (p[0], p[1] + 1) not in on: c = ramp[-1]
        px[p] = c

SMALL = {  # the credit, in lower case the game's font does not have
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "y": [".....", ".....", "#...#", "#...#", ".####", "....#", ".###."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####"],
    "p": [".....", ".....", "####.", "#...#", "####.", "#....", "#...."],
    "s": [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
    "k": ["#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."],
    "i": ["..#..", ".....", "..#..", "..#..", "..#..", "..#..", "..#.."],
    " ": ["....."] * 7,
}
def small_text(px, x, y, s, c, shadow):
    for k, ch in enumerate(s):
        for r, row in enumerate(SMALL[ch]):
            for q, v in enumerate(row):
                if v == "#":
                    px[x + k * 6 + q + 1, y + r + 1] = shadow
    for k, ch in enumerate(s):
        for r, row in enumerate(SMALL[ch]):
            for q, v in enumerate(row):
                if v == "#": px[x + k * 6 + q, y + r] = c

# ---------------------------------------------------------------- the warship
STEEL = [(240, 244, 240), (200, 212, 216), (160, 176, 188), (120, 136, 152), (88, 100, 120), (56, 64, 88), (32, 36, 56)]
RUST = [(184, 64, 48), (136, 40, 40), (88, 28, 36)]
WIN = (120, 216, 240)

def warship():
    """The one ship of all three screens: bow right, its forward guns raised. Returns an RGBA layer
    200x60 with the waterline at the bottom row, and the muzzle's position. No filter touches it."""
    im = Image.new("RGBA", (200, 60), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    p = im.load()
    def block(x0, y0, x1, y1, base=2):
        """Inclusive. Lit from the top right, as the sun sits there."""
        d.rectangle((x0, y0, x1, y1), fill=STEEL[base])
        d.line((x0, y0, x1, y0), fill=STEEL[max(base - 2, 0)])
        d.line((x1, y0, x1, y1), fill=STEEL[max(base - 1, 0)])
        d.line((x0, y0, x0, y1), fill=STEEL[min(base + 1, 6)])
        d.line((x0, y1, x1, y1), fill=STEEL[min(base + 2, 6)])

    # ---- things that stand behind the deck edge, back to front
    # mast and rigging
    d.line((110, 3, 110, 24), fill=STEEL[5])
    d.line((104, 9, 116, 9), fill=STEEL[5])
    d.line((106, 14, 114, 14), fill=STEEL[5])
    # the flag, flying aft
    for y, (x0, x1) in enumerate(((100, 109), (99, 109), (99, 109), (100, 109))):
        d.line((x0, y + 2, x1, y + 2), fill=RUST[0] if y < 2 else (240, 240, 232))
    p[100, 2] = (0, 0, 0, 0); p[99, 3] = RUST[1] + (255,)
    # funnel, raked aft
    for y in range(13, 30):
        rake = (29 - y) // 5
        d.line((78 - rake, y, 90 - rake, y), fill=STEEL[4])
        p[90 - rake, y] = STEEL[3] + (255,)
        p[78 - rake, y] = STEEL[5] + (255,)
    for y in range(13, 16): d.line((75, y, 87, y), fill=STEEL[6])
    for y in range(19, 22): d.line((76, y, 88, y), fill=RUST[0])

    # upper works
    block(70, 28, 132, 41, 2)
    for x in range(74, 130, 5): d.rectangle((x, 32, x + 2, 33), fill=STEEL[6])   # scuttles
    d.line((71, 37, 131, 37), fill=STEEL[3])
    block(86, 21, 124, 28, 1)
    for x in range(90, 122, 4): p[x, 24] = STEEL[6] + (255,); p[x + 1, 24] = WIN + (255,)
    block(98, 13, 122, 21, 1)                        # the bridge
    d.rectangle((102, 15, 121, 17), fill=STEEL[6])
    for x in range(103, 121, 3): d.rectangle((x, 15, x + 1, 16), fill=WIN)
    d.line((96, 12, 123, 12), fill=STEEL[0])
    block(106, 8, 116, 12, 2)
    d.rectangle((108, 5, 113, 7), fill=STEEL[4]); d.line((107, 4, 114, 4), fill=STEEL[1])   # radar
    # lifeboats on the upper works
    for x in (72, 126 - 2):
        d.rectangle((x, 26, x + 6, 27), fill=(232, 232, 216)); d.line((x + 1, 28, x + 5, 28), fill=(168, 112, 72))
    # small guns on the upper deck
    for x in (92, 118):
        d.rectangle((x - 2, 19, x + 2, 20), fill=STEEL[4])

    # aft turret, guns aft
    block(32, 33, 54, 41, 3)
    d.polygon([(36, 28), (50, 28), (54, 33), (32, 33)], fill=STEEL[2])
    d.line((36, 28, 50, 28), fill=STEEL[0])
    d.line((38, 30, 48, 30), fill=STEEL[1])
    d.line((14, 28, 34, 30), fill=STEEL[5]); d.line((14, 31, 34, 32), fill=STEEL[4])
    # second aft mount, smaller and higher
    block(56, 30, 68, 41, 3)
    d.line((42, 27, 58, 32), fill=STEEL[5])
    # forward turrets, guns raised towards the bow
    block(134, 33, 150, 41, 3)
    d.polygon([(136, 26), (148, 26), (152, 33), (134, 33)], fill=STEEL[2])
    d.line((136, 26, 148, 26), fill=STEEL[0])
    d.line((138, 28, 146, 28), fill=STEEL[1])
    d.line((148, 28, 176, 21), fill=STEEL[5]); d.line((148, 29, 176, 22), fill=STEEL[3])
    d.line((148, 31, 176, 24), fill=STEEL[5]); d.line((148, 32, 176, 25), fill=STEEL[4])

    # ---- the hull
    deck = lambda x: 44 - max(0, x - 120) * 0.13          # the sheer rises to the bow
    outline = [(4, 42), (10, 59), (172, 59), (196, 35), (160, 39), (120, 44)]
    mask = Image.new("1", im.size, 0)
    ImageDraw.Draw(mask).polygon(outline, fill=1)
    m = mask.load()
    for x in range(200):
        ys = [y for y in range(60) if m[x, y]]
        if not ys: continue
        top = ys[0]
        for y in ys:
            k = y - top
            if y >= 54: c = RUST[0] if y < 57 else RUST[1]
            elif k == 0: c = STEEL[0]
            elif k == 1: c = STEEL[1]
            elif k < 5: c = STEEL[2]
            elif y < 50: c = STEEL[3]
            else: c = STEEL[4]
            p[x, y] = c + (255,)
    # a shaded bow flare and a dark stern, and hull plating
    for x in range(12, 168, 26): d.line((x, 48, x, 53), fill=STEEL[4])
    for x in range(24, 162, 7):
        y = 47
        p[x, y] = STEEL[6] + (255,); p[x, y + 1] = STEEL[6] + (255,); p[x + 1, y] = WIN + (255,)
    d.rectangle((158, 45, 161, 47), fill=STEEL[6])                  # the hawse hole
    d.line((159, 48, 159, 52), fill=STEEL[5])                       # and its anchor chain
    # the deck rail, dotted
    for x in range(8, 192, 3):
        y = int(deck(x)) - 2
        if p[x, y][3] == 0: p[x, y] = STEEL[1] + (255,)
    return im, (178, 22)

def tinted(im, f):
    out = im.copy()
    p = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = p[x, y]
            if a: p[x, y] = f((r, g, b)) + (255,)
    return out

def paste(px, layer, x0, y0):
    q = layer.load()
    for y in range(layer.height):
        for x in range(layer.width):
            c = q[x, y]
            if c[3] and 0 <= x0 + x < W and 0 <= y0 + y < H: px[x0 + x, y0 + y] = c[:3]

def tilt(layer, degrees):
    """Tilt by shearing twice with hard steps, so no pixel is blended or smeared."""
    t = math.tan(math.radians(degrees))
    pad = int(abs(t) * 200) + 8
    out = Image.new("RGBA", (layer.width + pad * 2, layer.height + pad * 2), (0, 0, 0, 0))
    q, o = layer.load(), out.load()
    cx, cy = layer.width / 2, layer.height / 2
    for y in range(layer.height):
        for x in range(layer.width):
            if q[x, y][3] == 0: continue
            nx = x + pad + round((y - cy) * t * 0.5)
            ny = y + pad - round((x - cx) * math.sin(math.radians(degrees)))
            nx2 = nx + round((ny - pad - cy) * t * 0.5)
            if 0 <= nx2 < out.width and 0 <= ny < out.height: o[nx2, ny] = q[x, y]
    # shearing can leave pinholes: close them from the left neighbour
    for y in range(1, out.height - 1):
        for x in range(1, out.width - 1):
            if o[x, y][3] == 0 and o[x - 1, y][3] and o[x + 1, y][3]: o[x, y] = o[x - 1, y]
            elif o[x, y][3] == 0 and o[x, y - 1][3] and o[x, y + 1][3]: o[x, y] = o[x, y - 1]
    return out, pad

def flash(px, cx, cy, r, ramp=((255, 255, 216), (255, 216, 48), (248, 128, 24))):
    """A many-pointed muzzle star, flung forward."""
    n = 11
    for k in range(n):
        a = -0.9 + 1.8 * k / (n - 1)
        length = r * (1.0 if k % 2 else 0.55)
        for i in range(int(length)):
            x, y = round(cx + math.cos(a) * i), round(cy + math.sin(a) * i)
            if 0 <= x < W and 0 <= y < H:
                px[x, y] = ramp[2] if i > length * 0.6 else ramp[1]
    disc(px, cx, cy, max(2, r // 4), ramp[1])
    disc(px, cx, cy, max(1, r // 6), ramp[0])

def puff(px, cx, cy, r, light, mid, dark):
    """A hard-edged ball of smoke, lit from above."""
    disc(px, cx, cy, r, dark)
    disc(px, cx - r // 5, cy - r // 5, r - max(1, r // 4), mid)
    disc(px, cx - r // 3, cy - r // 3, max(1, r // 2), light)

def wake(px, x0, x1, y, c):
    """Foam along the waterline: broken runs, as a hull pushes through water."""
    for x in range(x0, x1):
        if (x * 7 + y * 3) % 11 < 7: px[x, y] = c
        if (x * 5) % 13 < 3: px[x, y + 1] = c

# ---------------------------------------------------------------- the three screens
def title():
    im = Image.new("RGB", (W, H)); px = im.load()
    sky(px, [(40, 88, 168), (56, 112, 192), (80, 144, 208), (120, 176, 224), (168, 208, 232), (224, 228, 216)], 0, 122)
    sun(px, 208, 62, 11, [(248, 240, 200), (252, 248, 216), (255, 248, 168), (255, 255, 232)])
    cloud(px, 2, 62, 40, 11, (248, 248, 240), (216, 224, 232), (160, 180, 208))
    cloud(px, 204, 104, 40, 9, (248, 248, 240), (216, 224, 232), (160, 180, 208))
    cloud(px, 2, 90, 26, 6, (240, 244, 240), (208, 220, 232), (168, 188, 212))
    ramp = [(255, 255, 255), (232, 240, 248), (200, 216, 232), (168, 192, 216), (136, 164, 196), (104, 132, 172)]
    logo(px, "NAVAL", 6, 4, ramp, (12, 28, 64), (20, 48, 100))
    logo(px, "HUNTER", 38, 4, ramp, (12, 28, 64), (20, 48, 100))
    sea(px, 122)
    ship, muzzle = warship()
    x0, y0 = 16, 128 - 59
    paste(px, ship, x0, y0)
    flash(px, x0 + muzzle[0], y0 + muzzle[1], 20)
    puff(px, x0 + muzzle[0] - 4, y0 + muzzle[1] - 4, 3, (240, 240, 232), (200, 204, 208), (150, 156, 168))
    wake(px, x0 + 4, x0 + 186, 128, (232, 248, 255))
    wake(px, x0 - 12, x0 + 12, 130, (200, 232, 248))
    swell(px, 149, 2, 44, 0.4, (10, 34, 72), (40, 96, 148), foam=(216, 240, 248))
    swell(px, 154, 2, 31, 2.1, (6, 22, 50), (24, 64, 112), foam=(168, 208, 232))
    small_text(px, (W - 9 * 6) // 2, 151, "By Zapski", (216, 232, 248), (0, 8, 24))
    return im

def firework(px, cx, cy, r, c1, c2):
    for k in range(16):
        a = 2 * math.pi * k / 16
        for i in range(r // 3, r, 2):
            x, y = round(cx + math.cos(a) * i), round(cy + math.sin(a) * i + i * i / (r * 8))
            if 0 <= x < W and 0 <= y < H: px[x, y] = c1 if i < r * 0.7 else c2

def win():
    im = Image.new("RGB", (W, H)); px = im.load()
    sky(px, [(40, 32, 96), (88, 48, 128), (160, 72, 128), (224, 112, 96), (248, 168, 88), (252, 216, 128)], 0, 118)
    sun(px, 188, 92, 19, [(252, 200, 112), (255, 224, 136), (255, 236, 160), (255, 250, 208)])
    for (x, y, w, h) in ((6, 80, 58, 9), (120, 62, 44, 6), (54, 60, 26, 5), (196, 72, 40, 6)):
        cloud(px, x, y, w, h, (255, 208, 160), (232, 136, 120), (136, 72, 120))
    firework(px, 28, 34, 22, (255, 224, 96), (248, 112, 96))
    firework(px, 214, 28, 18, (200, 240, 255), (136, 168, 248))
    firework(px, 66, 12, 12, (255, 200, 216), (240, 112, 160))
    ramp = [(255, 255, 224), (255, 240, 136), (255, 216, 72), (248, 176, 40), (224, 128, 24), (176, 80, 24)]
    logo(px, "YOU WIN", 14, 4, ramp, (64, 16, 40), (112, 32, 64))
    sea(px, 118)
    # the sun's track on the water, broken
    for y in range(118, 148):
        w = 3 + (y - 118) // 2
        for x in range(188 - w, 188 + w):
            if (x * 3 + y * 7) % 5 == 0 or (y & 1 and x % 3 == 0): px[x, y] = (255, 216, 128)
    warm = lambda c: mix(c, (248, 152, 88), 0.14)
    ship, muzzle = warship()
    ship = tinted(ship, warm)
    x0, y0 = 14, 124 - 59
    paste(px, ship, x0, y0)
    flash(px, x0 + muzzle[0], y0 + muzzle[1], 18)
    # signal flags from the mast to bow and stern: a ship dressed overall
    flags = [(232, 56, 48), (248, 216, 64), (64, 120, 216), (248, 248, 240), (56, 168, 96)]
    mast = (x0 + 110, y0 + 4)
    for end, sag in (((x0 + 12, y0 + 42), 5), ((x0 + 192, y0 + 34), 4)):
        n = abs(end[0] - mast[0])
        for i in range(0, n, 4):
            t = i / n
            x = round(mast[0] + (end[0] - mast[0]) * t)
            y = round(mast[1] + (end[1] - mast[1]) * t + sag * math.sin(math.pi * t))
            px[x, y] = (64, 40, 56)
            if i % 8 == 0:
                c = flags[(i // 8) % 5]
                px[x, y + 1] = c; px[x, y + 2] = c; px[x + 1, y + 1] = c
    wake(px, x0 + 4, x0 + 186, 124, (255, 232, 200))
    wake(px, x0 - 12, x0 + 12, 126, (240, 208, 184))
    swell(px, 138, 2, 52, 1.1, (28, 40, 96), (104, 96, 148), foam=(255, 224, 184))
    swell(px, 147, 2, 37, 3.0, (20, 24, 72), (64, 64, 120), foam=(248, 176, 136))
    return im

def lose():
    im = Image.new("RGB", (W, H)); px = im.load()
    sky(px, [(16, 8, 24), (40, 16, 40), (72, 24, 48), (120, 32, 48), (176, 56, 40), (216, 104, 48)], 0, 116)
    sun(px, 44, 96, 15, [(216, 80, 40), (232, 96, 40), (240, 120, 56), (248, 168, 80)])
    for (x, y, w, h) in ((110, 44, 70, 8), (4, 56, 50, 7), (176, 72, 50, 7), (60, 74, 36, 5)):
        cloud(px, x, y, w, h, (112, 48, 64), (72, 32, 56), (32, 16, 36))
    ramp = [(255, 216, 200), (248, 168, 144), (240, 104, 88), (216, 64, 56), (168, 40, 48), (112, 24, 40)]
    sea(px, 116)
    # darken the sea with a static screen, and let the fire glint in it
    for y in range(116, H):
        for x in range(W):
            if (x + y) & 1 == 0 and y < 150: px[x, y] = (10, 22, 56)
    for y in range(116, 146):
        w = 3 + (y - 116) // 2
        for x in range(44 - w, 44 + w):
            if (x * 3 + y * 7) % 5 == 0: px[x, y] = (232, 104, 56)
    dark = lambda c: mix(c, (24, 12, 32), 0.45)
    ship, _ = warship()
    # scorched: rub out most of the window glow, keep the shape
    ship = tinted(ship, lambda c: dark((180, 60, 40)) if c == WIN else dark(c))
    layer, pad = tilt(ship, 17)
    x0, y0 = 22 - pad, 128 - 59 - pad - 8
    paste(px, layer, x0, y0)
    # smoke, biggest high up, drifting away from the funnel
    balls = [(142, 58, 16), (160, 46, 13), (126, 44, 12), (178, 36, 10), (146, 34, 9), (120, 60, 9), (196, 30, 7), (108, 66, 6), (210, 24, 5)]
    for (x, y, r) in sorted(balls, key=lambda b: -b[1]):
        puff(px, x, y, r, (96, 80, 88), (56, 44, 60), (28, 20, 36))
    # flames on the deck, tall on the lit side
    def flame(x, y, h):
        for i in range(h):
            w = max(0, (h - i) // 3)
            for dx in range(-w, w + 1):
                c = (255, 255, 200) if abs(dx) < w / 2 and i < h / 3 else (255, 200, 48) if i < h * 0.6 and abs(dx) < w else (232, 88, 24)
                px[x + dx + (i % 2), y - i] = c
    for (x, y, h) in ((64, 104, 13), (92, 96, 16), (122, 86, 12), (52, 110, 9), (150, 80, 8), (108, 90, 9)):
        flame(x, y, h)
    # the sea closes over the stern: spikes of spray along the waterline, as on the first game
    for (x, h) in ((14, 9), (30, 13), (46, 16), (62, 13), (84, 9), (112, 7), (136, 11), (160, 15), (178, 10), (198, 6)):
        for i in range(h):
            w = (h - i) // 3
            for dx in range(-w, w + 1):
                px[x + dx, 128 - i] = (216, 232, 240) if i > h // 2 or dx == 0 else (120, 160, 192)
    swell(px, 126, 2, 34, 0.7, (8, 18, 44), (48, 72, 112), y1=136, foam=(200, 224, 232))
    swell(px, 134, 2, 48, 2.4, (10, 22, 56), (56, 40, 72), y1=142, foam=(232, 120, 72))
    sea(px, 142, 152)
    for y in range(142, 152):
        for x in range(W):
            if (x + y) & 1 == 0: px[x, y] = (10, 22, 56)
    swell(px, 150, 2, 37, 1.3, (6, 14, 36), (40, 40, 72), foam=(176, 112, 96))
    logo(px, "YOU LOSE", 8, 4, ramp, (16, 4, 16), (48, 8, 24))
    return im

# ---------------------------------------------------------------- output
def index_image(im, name):
    """RGB plus the sea's key colours to indexes, and this screen's palette."""
    px = im.load()
    pal, out = {}, bytearray(W * H)
    shared = {snap(c): 16 + i for i, (_, c) in enumerate(SHARED)}
    for y in range(H):
        for x in range(W):
            c = px[x, y]
            if c[:2] == (1, 2): out[y * W + x] = c[2]; continue
            c = snap(c)
            if c in shared: out[y * W + x] = shared[c]; continue
            if c not in pal: pal[c] = ART_FIRST + len(pal)
            out[y * W + x] = pal[c]
    assert len(pal) <= ART_N, f"{name}: {len(pal)} colours, only {ART_N} slots"
    print(f"{name}: {len(pal)} of {ART_N} palette slots")
    return bytes(out), list(pal)

def c_bytes(b, per):
    return ",\n".join(",".join(str(v) for v in b[i:i + per]) for i in range(0, len(b), per))

def flat(tiles):
    return bytes(v for t in tiles for r in t for v in r)

def sheet(tiles, s, cols, full):
    """Tiles on a sheet, for the preview."""
    n = len(tiles)
    im = Image.new("RGB", (cols * (s + 2) + 2, (n // cols + 1) * (s + 2) + 2), (60, 60, 60))
    for k, t in enumerate(tiles):
        x0, y0 = 2 + k % cols * (s + 2), 2 + k // cols * (s + 2)
        for y, r in enumerate(t):
            for x, v in enumerate(r):
                im.putpixel((x0 + x, y0 + y), full[v])
    return im

def main():
    screens = {"title": title(), "win": win(), "lose": lose()}
    data = {n: index_image(im, n) for n, im in screens.items()}
    big = tile_set(12, HULL_BIG, JOINT_BIG, MARK_BIG)
    mini = tile_set(8, HULL_MINI, JOINT_MINI, MARK_MINI)
    icons = [[icon(n, s) for s in (0, 1)] for n in range(1, 6)]

    shared_pal = [snap(c) for c in SEA] + [snap(c) for _, c in SHARED]
    shared_pal += [(0, 0, 0)] * (ART_FIRST - len(shared_pal))

    h = ["/* Generated by tools/gen_art.py. Do not edit: change the script and run it. */",
         "#ifndef ART_H_INCLUDED", "#define ART_H_INCLUDED", "#include <stdint.h>", ""]
    h.append("enum { " + ", ".join(f"{n} = {SHARED_IDX[n]}" for n, _ in SHARED) + " };")
    h.append("/* A hull cell, from gfx_segment(). Bows point right and up. */")
    h.append("enum { " + ", ".join(f"SEG_{s} = {i}" for i, s in enumerate(SEGS)) + " };")
    h.append("/* A board cell's art: T_HULL + segment, and the same for T_HULLHIT, T_SUNK and T_BAD. */")
    h.append("enum { " + ", ".join(f"{n} = {v}" for n, v in T.items()) + " };")
    h.append(f"enum {{ ART_PAL_FIRST = {ART_FIRST}, ART_PAL_N = {ART_N}, ICON_W = {ICON_W}, ICON_H = {ICON_H} }};")
    h += ["extern const uint16_t art_shared_pal[ART_PAL_FIRST];"]
    for n in screens:
        h += [f"extern const uint16_t art_{n}_pal[ART_PAL_N];", f"extern const uint8_t art_{n}[{W * H}];"]
    obj_data, obj_enum = bytearray(), []
    for name, frames in OBJ_FRAMES:
        obj_enum.append(f"{name} = {len(obj_data) // 32}")
        obj_enum.append(f"{name}_N = {len(frames)}")
        for g in frames: obj_data += obj_tiles(g)
    h.append("/* Sprite art: a frame's first tile, from where main.c loads it. Frames are 4 tiles but for OBJ_SMOKE's. */")
    h.append("enum { " + ", ".join(obj_enum) + f", OBJ_TILES = {len(obj_data) // 32}" + " };")
    h += ["extern const uint16_t art_obj_pal[16];", "extern const uint8_t art_obj[OBJ_TILES * 32];"]
    h += ["extern const uint8_t art_big[T_COUNT][144];", "extern const uint8_t art_mini[T_COUNT][64];",
          "/* [length - 1][sunk], a row at a time */", "extern const uint8_t art_icon[5][2][ICON_W * ICON_H];", "", "#endif", ""]
    with open(os.path.join(ROOT, "src/art.h"), "w") as f: f.write("\n".join(h))

    c = ["/* Generated by tools/gen_art.py. Do not edit: change the script and run it. */", '#include "art.h"', ""]
    A4 = "__attribute__((aligned(4)))"
    c.append(f"const uint16_t art_shared_pal[ART_PAL_FIRST] = {{{','.join(str(rgb15(x)) for x in shared_pal)}}};")
    for n, (pix, pal) in data.items():
        full = [rgb15(x) for x in pal] + [0] * (ART_N - len(pal))
        c.append(f"const uint16_t art_{n}_pal[ART_PAL_N] = {{{','.join(map(str, full))}}};")
        c.append(f"const uint8_t art_{n}[{W * H}] {A4} = {{\n{c_bytes(pix, W)}\n}};")
    c.append(f"const uint8_t art_big[T_COUNT][144] {A4} = {{\n" + ",\n".join("{" + ",".join(map(str, sum(t, []))) + "}" for t in big) + "\n};")
    c.append(f"const uint8_t art_mini[T_COUNT][64] {A4} = {{\n" + ",\n".join("{" + ",".join(map(str, sum(t, []))) + "}" for t in mini) + "\n};")
    ic = ",\n".join("{" + ",".join("{" + ",".join(map(str, sum(s, []))) + "}" for s in n) + "}" for n in icons)
    c.append(f"const uint8_t art_icon[5][2][ICON_W * ICON_H] = {{\n{ic}\n}};")
    c.append(f"const uint16_t art_obj_pal[16] = {{{','.join(str(rgb15(x)) for x in OBJ_PAL)}}};")
    c.append(f"const uint8_t art_obj[OBJ_TILES * 32] {A4} = {{\n{c_bytes(obj_data, 32)}\n}};")
    with open(os.path.join(ROOT, "src/art.c"), "w") as f: f.write("\n".join(c) + "\n")

    # ---- the preview: the screens at 1x, then every cell and every icon enlarged
    def rendered(n):
        pix, pal = data[n]
        table = [snap(x) for x in SEA] + [snap(x) for _, x in SHARED] + [(0, 0, 0)] * (ART_FIRST - 16 - len(SHARED)) + pal
        table += [(0, 0, 0)] * (256 - len(table))
        im = Image.new("RGB", (W, H))
        im.putdata([table[v] for v in pix])
        return im
    full = shared_pal + [(0, 0, 0)] * 192
    bs, ms = sheet(big, 12, 16, full), sheet(mini, 8, 16, full)
    prev = Image.new("RGB", (W * 3 + 16, H * 2 + 8 + bs.height * 4 + ms.height * 4 + 90 + 70), (60, 60, 60))
    for k, n in enumerate(screens):
        prev.paste(rendered(n), (k * (W + 8), 0))
    y = H + 8
    prev.paste(bs.resize((bs.width * 4, bs.height * 4), Image.NEAREST), (0, y))
    y += bs.height * 4 + 4
    prev.paste(ms.resize((ms.width * 4, ms.height * 4), Image.NEAREST), (0, y))
    y += ms.height * 4 + 4
    for n in range(5):
        for s in (0, 1):
            t = icons[n][s]
            ic_im = Image.new("RGB", (ICON_W, ICON_H))
            ic_im.putdata([full[v] for r in t for v in r])
            prev.paste(ic_im.resize((ICON_W * 3, ICON_H * 3), Image.NEAREST), (s * (ICON_W * 3 + 6), y + n * (ICON_H * 3 + 2)))
    y += 5 * (ICON_H * 3 + 2) + 4
    ox = 0
    for _, frames in OBJ_FRAMES:                   # every sprite frame, on the grey of this sheet
        for g in frames:
            n = len(g)
            spr = Image.new("RGB", (n, n), (60, 60, 60))
            for py, r in enumerate(g):
                for qx, v in enumerate(r):
                    if v: spr.putpixel((qx, py), snap(OBJ_PAL[v]))
            prev.paste(spr.resize((n * 3, n * 3), Image.NEAREST), (ox, y))
            ox += n * 3 + 4
    y += 16 * 3
    prev = prev.crop((0, 0, prev.width, y))
    prev.save(os.path.join(ROOT, "tools/art.png"))
    # for looking at each screen closely while drawing
    if os.environ.get("ART_BIG"):
        for n in screens: rendered(n).resize((W * 3, H * 3), Image.NEAREST).save(os.path.join(os.environ["ART_BIG"], n + ".png"))

if __name__ == "__main__":
    main()
