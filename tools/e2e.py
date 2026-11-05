#!/usr/bin/env python3
"""Drives the real ROM in libmgba (tools/emu.c) with real key presses. What it pins is what
only the ROM can show. Boards are set up by poke, never by a test build. Screenshots go to
shots/. Exit 1 on any FAIL."""
import os, re, subprocess, sys
from PIL import Image

DISPCNT, VRAM, OAM, PAL = 0x04000000, 0x06000000, 0x07000000, 0x05000000
SND4ENV, SND4POLY, SNDSTAT, SND1ENV = 0x04000079, 0x0400007C, 0x04000084, 0x04000063
ST_BOOT, ST_TITLE, ST_MENU, ST_PLACE, ST_BATTLE, ST_ENEMY, ST_END, ST_SPLASH = range(8)
# The palette (C_*), hull segments (SEG_*) and cell art (T_*), as generated
globals().update({n: int(v) for n, v in re.findall(r"(\w+) = (\d+)", open("src/art.h").read())})
A, B, SELECT, START, RIGHT, LEFT, UP, DOWN = 1, 2, 4, 8, 16, 32, 64, 128
S_EMPTY, S_MISS, S_HIT, S_SUNK = range(4)
EMPTY = 255
SHIP_LEN = [5, 4, 3, 2, 2, 1, 1]
BIG, SMALL = (12, 22, 12), (144, 12, 8)
PANEL_X, PANEL_Y = 144, 103
ICON_DX = 63                # main.c's ICON_X; odd
START_X, START_Y = 87, 133  # main.c's PRESS START: odd x
SPLASH_FRAMES, SPLASH_LOCK = 300, 36
FRAME_CYCLES = 280896
THINK_FRAMES = 30

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
    def u16(self, name): return int.from_bytes(self.peek(self.sym[name], 2), "little")
    def u32(self, name): return int.from_bytes(self.peek(self.sym[name], 4), "little")
    def text(self, name): return self.peek(self.sym[name], 24).split(b"\0")[0].decode()
    def reg(self, a): return int.from_bytes(self.peek(a, 2), "little")
    def tap(self, key): self.run(2, key); self.run(2)
    def px(self, x, y): return self.peek(VRAM + y * 240 + x)[0]
    def shot(self, name):
        os.makedirs("shots", exist_ok=True)
        self.cmd(f"shot shots/{name}.ppm")
        im = Image.open(f"shots/{name}.ppm").resize((480, 320), Image.NEAREST)
        im.save(f"shots/{name}.png")
        os.remove(f"shots/{name}.ppm")
        return im.convert("RGB")
    def until(self, pred, limit=3000, keys=0):
        for _ in range(limit):
            if pred(): return True
            self.run(1, keys)
        return False
    def state(self): return self.u8("state")
    def wait_state(self, st, limit=3000): return self.until(lambda: self.state() == st, limit)
    # Board: own[100] st[100] hits[7] alive
    def board(self, name):
        b = self.peek(self.sym[name], 208)
        return b[:100], b[100:200], b[200:207], b[207]
    def poke_board(self, name, own, st, hits, alive):
        self.poke(self.sym[name], bytes(own) + bytes(st) + bytes(hits) + bytes([alive]))
    def fb(self): return self.peek(VRAM, 240 * 160)
    def pal(self, first, n): return self.peek(PAL + first * 2, n * 2)
    def grab(self):
        """The screen as it is, at 240x160, with no file left behind."""
        self.cmd("shot shots/.grab.ppm")
        im = Image.open("shots/.grab.ppm").convert("RGB")
        os.remove("shots/.grab.ppm")
        return im
    def art(self, name, n): return self.peek(self.sym[name], n)
    def tile_art(self, pane):
        """Every T_* tile of a pane's size, as bytes, from the ROM."""
        s = pane[2]
        data = self.art("art_big" if s == 12 else "art_mini", T_COUNT * s * s)
        return [data[t * s * s:(t + 1) * s * s] for t in range(T_COUNT)]
    def tiles(self, pane):
        """Which T_* each of a pane's cells holds in VRAM, or None if it is not exactly one of them."""
        fb, (px, py, s) = self.fb(), pane
        known = {b: t for t, b in enumerate(self.tile_art(pane))}
        return [known.get(b"".join(fb[(py + i // 10 * s + r) * 240 + px + i % 10 * s:][:s] for r in range(s)))
                for i in range(100)]
    def redraw(self, pane):
        """Forget what a pane has drawn, so it repaints every cell. Pane.shown is at offset 17 in main.c."""
        self.poke(self.sym["big" if pane == BIG else "mini"] + 17, bytes([0xFF] * 100))
    def panel_row(self, s):
        y = PANEL_Y + 8 * s
        return self.peek(VRAM + y * 240 + PANEL_X, 92)
    def goto(self, x, y):
        for key, get, want in ((RIGHT, "cx", x), (DOWN, "cy", y)):
            while self.u8(get) != want: self.tap(key)
    def shoot(self, i):
        self.goto(i % 10, i // 10)
        self.tap(A)
    def battle_ready(self):
        return self.wait_state(ST_BATTLE) and self.until(lambda: self.text("status_text") == "YOUR TURN", 30)

def seg(own, i):
    """gfx_segment's rule, written again from the brief: a cell's segment from its same-id neighbours."""
    x, y, id = i % 10, i // 10, own[i]
    l, r = x > 0 and own[i - 1] == id, x < 9 and own[i + 1] == id
    u, d = y > 0 and own[i - 10] == id, y < 9 and own[i + 10] == id
    if l or r: return SEG_H_STERN if not l else SEG_H_BOW if not r else SEG_H_MID
    if u or d: return SEG_V_BOW if not u else SEG_V_STERN if not d else SEG_V_MID
    return SEG_BOAT

def want_tiles(own, st, view):
    """What a board must show. view: 'hidden' (the enemy in battle), 'ships' (yours), 'end'."""
    out = []
    for i in range(100):
        if st[i] == S_SUNK: t = T_SUNK + seg(own, i)
        elif st[i] == S_HIT: t = T_HIT if view == "hidden" else T_HULLHIT + seg(own, i)
        elif st[i] == S_MISS and view != "end": t = T_MISS
        elif view != "hidden" and own[i] != EMPTY: t = T_HULL + seg(own, i)
        elif view != "end" and st[i] == S_EMPTY and any(st[n] == S_SUNK for n in around(i)): t = T_KNOWN
        else: t = T_WATER
        out.append(t)
    return out

def is_black(im): return im.getextrema() == ((0, 0), (0, 0), (0, 0))

def dims(frame, ref):
    """frame could be ref at some brightness: nowhere brighter, and each colour of ref goes to one colour."""
    m = {}
    fb, rb = frame.tobytes(), ref.tobytes()
    for i in range(0, len(fb), 3):
        a, b = fb[i:i + 3], rb[i:i + 3]
        if a[0] > b[0] or a[1] > b[1] or a[2] > b[2] or m.setdefault(b, a) != a: return False
    return True

def watch(e, done, keys=0, limit=120):
    """A frame at a time until done(): every frame the player saw."""
    frames = []
    while not done() and len(frames) < limit:
        e.run(1, keys)
        frames.append(e.grab())
    return frames

def check_fade(what, start, frames):
    """Through a change of picture, no frame shows wrong colours: the old picture dims to black, and
    only then does the new one brighten from black. start is None if the old side has its own checks."""
    black = [i for i, f in enumerate(frames) if is_black(f)]
    if not black:
        check(False, f"{what}: it goes through black"); return
    final = frames[-1]
    if start: check(all(dims(f, start) for f in frames[:black[0]]), f"{what}: the old picture only dims ({black[0]} frames)")
    check(all(is_black(f) for f in frames[black[0]:black[-1] + 1]), f"{what}: then black, unbroken ({black[-1] - black[0] + 1} frames)")
    check(all(dims(f, final) for f in frames[black[-1] + 1:]), f"{what}: then the new picture only brightens ({len(frames) - black[-1] - 1} frames)")

def shared_range_kept(e, where):
    check(e.pal(0, ART_PAL_FIRST) == e.art("art_shared_pal", ART_PAL_FIRST * 2), f"{where}: palette 0-63 is the shared one")

def chrome_clear_of_sea(fb, what):
    """Nothing but water sits in the sea range. Everything outside the boards' cells must be above it."""
    def in_cell(x, y):
        for px, py, s in (BIG, SMALL):
            if px < x < px + 10 * s and py < y < py + 10 * s: return True
        return False
    bad = [(x, y) for y in range(160) for x in range(240) if fb[y * 240 + x] < 16 and not in_cell(x, y)]
    check(not bad, f"{what}: no chrome pixel in the sea range {bad[:3]}")

def cells(own, s): return [i for i in range(100) if own[i] == s]
def water(own): return [i for i in range(100) if own[i] == EMPTY]

def around(i):
    """The 8 cells next to cell i, on the board."""
    return [(i // 10 + dy) * 10 + i % 10 + dx for dy in (-1, 0, 1) for dx in (-1, 0, 1)
            if (dx or dy) and 0 <= i % 10 + dx <= 9 and 0 <= i // 10 + dy <= 9]

def legal_fleet(own):
    for i in range(100):
        if own[i] == EMPTY: continue
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                x, y = i % 10 + dx, i // 10 + dy
                if 0 <= x <= 9 and 0 <= y <= 9 and own[y * 10 + x] not in (EMPTY, own[i]): return False
    return all(len(cells(own, s)) == SHIP_LEN[s] for s in range(7))

def enter_battle(e):
    """From the menu, deal a random game and wait for the first turn."""
    e.tap(A)
    return e.battle_ready()

def last_one_alive(e, name, also_fired):
    """Sink all but the last sub of a fleet by poke. also_fired: every other cell is a spent miss too."""
    own, st, hits, _ = e.board(name)
    st, hits = bytearray(st), bytearray(hits)
    last = cells(own, 6)[0]
    for i in range(100):
        if own[i] != EMPTY and own[i] != 6: st[i] = S_SUNK
        elif own[i] == EMPTY and also_fired: st[i] = S_MISS
    for s in range(6): hits[s] = SHIP_LEN[s]
    e.poke_board(name, own, st, hits, 1)
    return last

# sim.h's dealing, ported so the ROM's fleets can be checked against the seed it used.
def deal_from(seed):
    """Returns deal(), which gives the next fleet from that seed in sim_rand's order."""
    state = [seed or 1]
    def rand():
        x = state[0]
        x ^= x << 13 & 0xFFFFFFFF
        x ^= x >> 17
        x ^= x << 5 & 0xFFFFFFFF
        state[0] = x
        return x
    def fits(own, x, y, n, horiz):
        for k in range(n):
            cx, cy = (x + k, y) if horiz else (x, y + k)
            if cx > 9 or cy > 9: return False
            if any(0 <= cx + dx <= 9 and 0 <= cy + dy <= 9 and own[(cy + dy) * 10 + cx + dx] != EMPTY
                   for dy in (-1, 0, 1) for dx in (-1, 0, 1)): return False
        return True
    def deal():
        while True:
            own = [EMPTY] * 100
            for s, n in enumerate(SHIP_LEN):
                for _ in range(201):
                    horiz, x, y = rand() & 1, rand() % 10, rand() % 10
                    if fits(own, x, y, n, horiz):
                        for k in range(n): own[y * 10 + x + k if horiz else (y + k) * 10 + x] = s
                        break
                else: break
            else: return bytes(own)
    return deal

def clever_score(own):
    v = 0
    moat = set()
    for i in range(100):
        if own[i] != EMPTY:
            x, y = i % 10, i // 10
            v += min(x, 9 - x) + min(y, 9 - y)
            moat |= {i} | set(around(i))
    return v + 2 * len(moat)

def plain_deal_check(e, what):
    """A non-ADMIRAL game: both fleets are what the plain deal gives from this game's seed."""
    deal = deal_from(e.u32("game_seed"))
    want_o, want_p = deal(), deal()
    check(e.board("ob")[0] == want_o and e.board("pb")[0] == want_p, f"{what}: both fleets are the plain deal from seed {e.u32('game_seed'):#x}")

def clever_deal_check(e, what):
    """An ADMIRAL game: the enemy keeps the lowest score of 4 deals, and yours is the next plain deal."""
    deal = deal_from(e.u32("game_seed"))
    want_o = min((deal() for _ in range(4)), key=clever_score)   # min keeps the first on a tie, as sim.h does
    want_p = deal()
    own_o, own_p = e.board("ob")[0], e.board("pb")[0]
    check(own_o == want_o, f"{what}: the enemy fleet is the best of 4 deals from seed {e.u32('game_seed'):#x}")
    check(own_p == want_p, f"{what}: your fleet is the deal after them")
    check(legal_fleet(own_o) and legal_fleet(own_p), f"{what}: both are legal")

def admiral_game(e, midshot=None):
    """Lose a game to ADMIRAL by only shooting water. Every enemy shot is checked as it lands.
    Returns (turn, samples, tries) per turn, or None if the game stalled."""
    own_o = e.board("ob")[0]
    rows, drops0 = [], e.u32("frame_drops")
    for turn, i in enumerate(water(own_o), 1):
        if not e.battle_ready(): return None
        st0 = e.board("pb")[1]
        e.shoot(i)
        if not e.until(lambda: e.text("status_text") == "ENEMY TURN", 200): return None
        e.poke(e.sym["worst_frame"], bytes(4))
        if not e.until(lambda: e.board("pb")[1] != st0, THINK_FRAMES + 2): return None
        e.run(2)                                # the frame that picks and fires has ended
        st1, le = e.board("pb")[1], e.u8("last_enemy")
        live = [c for c in range(100) if st0[c] == S_EMPTY and not any(st0[n] == S_SUNK for n in around(c))]
        rows.append(dict(turn=turn, samples=e.u16("samples"), tries=e.u16("sample_tries"), think=e.u8("think_frames"),
                         spent=st0[le] != S_EMPTY, water=bool(live) and le not in live, cost=e.u32("worst_frame")))
        if midshot and turn == midshot[0]: e.shot(midshot[1])
        if e.board("pb")[3] == 0: break
    check(e.u32("frame_drops") == drops0, f"no frame dropped through it ({e.u32('frame_drops') - drops0})")
    return rows

def main():
    e = Emu("navalhunter.gba")
    e.run(10)

    # --- title: only START leaves
    check(e.state() == ST_TITLE, f"booted to the title: state {e.state()}")
    d = e.reg(DISPCNT)
    check(d & 7 == 4, f"Mode 4 in REG_DISPCNT ({d:#06x})")
    check(d & 0x10 == 0, "showing page 0")
    check(d & 0x400 and d & 0x1000, "BG2 and sprites on")
    check(d & 0x80 == 0, "not force-blanked")
    n = ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."]
    got = ["".join("#" if e.px(START_X + x, START_Y + y) == C_YEL else "." for x in range(5)) for y in range(7)]
    check(got == n, f"text draws the font's P of PRESS START, at odd x: {got}")
    e.shot("1-title")
    title = e.art("art_title", 38400)
    fb = e.fb()
    diff = {(i % 240, i // 240) for i in range(38400) if fb[i] != title[i]}
    check(diff and all(START_X <= x < START_X + 66 and START_Y <= y < START_Y + 8 for x, y in diff),
          f"the title is the art, pixel for pixel, but for the words ({len(diff)} pixels)")
    check(max(title) > ART_PAL_FIRST and min(title) < 16, "and it uses its own colours and the sea's")
    check(e.pal(ART_PAL_FIRST, ART_PAL_N) == e.art("art_title_pal", ART_PAL_N * 2), "its palette is loaded")
    shared_range_kept(e, "title")
    sea_row = title[140 * 240:141 * 240]
    check(min(sea_row) < 16, f"the title's water is in the sea range: row 140 has {sum(v < 16 for v in sea_row)} of 240")
    for key in (A, B, SELECT, RIGHT, LEFT, UP, DOWN):
        e.tap(key)
    e.run(60)
    check(e.state() == ST_TITLE, "title holds through A, B, SELECT and the d-pad")
    seen = set()
    off = None
    for _ in range(130):
        v = e.px(START_X, START_Y)
        seen.add(v)
        if v != C_YEL and off is None:
            off = e.fb()
            e.shot("1b-title-off")
        e.run()
    check(seen == {C_YEL, title[START_Y * 240 + START_X]}, f"PRESS START blinks: colours {sorted(seen)}")
    # the blink's erase is a gfx_blit from odd x to even x, whose last column holds the words'
    # shadow: a slip at either edge shows here
    check(off == title, "and the off phase puts back the art exactly, odd x and even")
    check(e.u32("frame_drops") == 0, f"no frame dropped on the title ({e.u32('frame_drops')})")
    f0 = e.u32("frames")
    e.run(300)
    check(e.u32("frames") - f0 == 300, f"VBlank IRQ ran for all 300 frames ({e.u32('frames') - f0})")
    while e.px(START_X, START_Y) != C_YEL: e.run()
    start = e.grab()
    e.run(1, START)
    frames = watch(e, lambda: e.state() == ST_MENU, START)
    check(e.state() == ST_MENU, f"START leaves the title: state {e.state()}")
    check_fade("title to menu", start, frames)
    shared_range_kept(e, "menu")

    # --- menu: SELECT cycles the level, other keys do nothing
    e.run(5)
    chrome_clear_of_sea(e.fb(), "menu")
    q = sum(e.px((240 - 6 * 17) // 2 + 16 * 6 + x, 70 + y) == C_INK for x in range(5) for y in range(7))
    check(q > 0, f"the ? of the menu's question is drawn ({q} pixels)")
    e.shot("2-menu")
    levels = [e.u8("ai_level")]
    for _ in range(4):
        e.tap(SELECT); levels.append(e.u8("ai_level"))
        if levels[-1] == 3:
            e.run(2)
            e.shot("2b-menu-admiral")
    check(levels == [1, 2, 3, 0, 1], f"SELECT cycles four levels: {levels}")
    e.tap(START); e.tap(RIGHT); e.run(5)
    check(e.state() == ST_MENU, "START and the d-pad do not leave the menu")

    # --- manual placement
    e.tap(B)
    check(e.state() == ST_PLACE, f"B on the menu starts placement: state {e.state()}")
    e.run(5)
    check(all(set(e.panel_row(s)) == {C_BG} for s in range(7)), "fleet list empty before any ship is placed")
    got = e.tiles(BIG)
    check(got[:6] == [T_HULL + SEG_H_STERN, T_HULL + SEG_H_MID, T_HULL + SEG_H_MID, T_HULL + SEG_H_MID, T_HULL + SEG_H_BOW, T_WATER], f"carrier preview shown legal at A1, stern to bow: {got[:6]}")
    e.shot("3-place-start")
    e.tap(A)                                    # carrier along the top row
    e.run(5)
    own = e.board("pb")[0]
    check(cells(own, 0) == [0, 1, 2, 3, 4] and e.u8("place_s") == 1, f"carrier committed: {cells(own, 0)}")
    check(set(e.panel_row(0)) != {C_BG} and set(e.panel_row(1)) == {C_BG}, "fleet list shows only the ship placed")

    # refused: the battleship's preview sits on the carrier
    got = e.tiles(BIG)
    check(got[0] == T_BAD + SEG_H_STERN and got[3] == T_BAD + SEG_H_BOW, f"illegal position shown in the refused art: {got[:4]}")
    e.shot("4-place-illegal")
    e.tap(A)
    e.run(1)
    check(e.u8("place_s") == 1 and cells(e.board("pb")[0], 1) == [], "illegal position refused: no ship placed")
    check(e.text("msg_text") == "TOO CLOSE", f"refusal says so: {e.text('msg_text')!r}")
    e.run(30)

    # rotate, then move clear of the moat (row 1 touches the carrier, row 2 does not)
    e.tap(B)
    check(e.u8("place_horiz") == 0, "B rotates to vertical")
    e.tap(DOWN); e.run(2)
    got = e.tiles(BIG)
    check(got[10] == T_BAD + SEG_V_BOW and got[40] == T_BAD + SEG_V_STERN, f"a touching position is refused too (row 2): {got[10]} {got[40]}")
    e.tap(A); e.run(31)
    check(e.u8("place_s") == 1, "touching position not committed")
    e.tap(DOWN); e.run(2)
    got = e.tiles(BIG)
    check(got[20] == T_HULL + SEG_V_BOW and got[30] == T_HULL + SEG_V_MID and got[50] == T_HULL + SEG_V_STERN, f"vertical preview legal at A3, bow up: {got[20]} {got[30]} {got[50]}")
    e.tap(A); e.run(2)
    check(cells(e.board("pb")[0], 1) == [20, 30, 40, 50], f"battleship committed vertically: {cells(e.board('pb')[0], 1)}")
    e.shot("5-place-progress")

    # key repeat, on the placement cursor: at once, then after 18 frames, then every 10
    x0 = e.u8("place_x"); e.run(2)
    e.run(1, RIGHT)
    moved = [0, e.u8("place_x") - x0]
    for _ in range(30):
        e.run(1, RIGHT); moved.append(e.u8("place_x") - x0)
    e.run(2)
    steps = [i for i in range(1, len(moved)) if moved[i] != moved[i - 1]]
    check(len(steps) == 3 and steps[1] - steps[0] == 19 and steps[2] - steps[1] == 10,
          f"key repeat: a step at once, +19 frames, then +10 (the GB's 18 and 9): {steps}")

    # START randomises the rest
    placed = e.board("pb")[0]
    e.tap(START)
    check(e.battle_ready(), "START randomises the rest and battle starts")
    own, st, hits, alive = e.board("pb")
    check(legal_fleet(own) and alive == 7, f"all seven placed legally, alive {alive}")
    check(cells(own, 0) == cells(placed, 0) and cells(own, 1) == cells(placed, 1), "the manual ships stayed put")

    # --- battle: first screen
    e.run(10)
    own_o = e.board("ob")[0]
    check(legal_fleet(own_o), "enemy fleet is legal")
    check(e.tiles(BIG) == [T_WATER] * 100, "enemy ships hidden: the big board is all water")
    check(e.tiles(SMALL) == want_tiles(own, st, "ships"), "your ships always shown, each cell its segment")
    # every shape is apart from the others: marks must read without colour
    pal = e.pal(0, 64)
    def bright(v):
        c = pal[v * 2] | pal[v * 2 + 1] << 8
        return 2 * (c & 31) + 5 * (c >> 5 & 31) + (c >> 10 & 31) > 96      # a grey level of about 40%, weights 2:5:1
    for pane in (BIG, SMALL):
        ta, s = e.tile_art(pane), pane[2]
        inner = lambda t: [v >= 16 for r in range(1, s) for v in ta[t][r * s + 1:(r + 1) * s]]
        check(not any(inner(T_WATER)) and not set(ta[T_WATER][s + 1:]) - set(range(16)) - {C_GRID}, f"{s}px open water is all in the sea range")
        check(len({tuple(inner(t)) for t in (T_WATER, T_MISS, T_HIT, T_KNOWN)}) == 4, f"{s}px water, miss, hit and known water differ in shape, not just colour")
        grey = lambda t: tuple(bright(v) for v in ta[t])
        check(all(len({grey(T_HULL + g), grey(T_HULLHIT + g), grey(T_SUNK + g)}) == 3 for g in range(7)), f"{s}px hull, hit hull and sunk hull differ in black and white")
        check(len(set(ta)) == T_COUNT, f"{s}px all {T_COUNT} tiles are different")
    chrome_clear_of_sea(e.fb(), "battle")
    shared_range_kept(e, "battle")
    check(all(set(e.panel_row(s)) != {C_BG} for s in range(7)), "enemy fleet list full in battle")

    # Phase 1's bug: a byte write lands in both bytes of a halfword. Pixels the board draws:
    # the grid line (even x) drawn first, then the cell (odd x) beside it.
    grid_x, sea_x = BIG[0] + 12 * 5, BIG[0] + 12 * 5 + 1      # clear of the cursor at A1
    y = BIG[1] + 5
    water_row = e.tile_art(BIG)[T_WATER][5 * 12:6 * 12]
    check(e.px(grid_x, y) == C_GRID, f"even-x grid pixel ({grid_x},{y}) correct in VRAM: {e.px(grid_x, y)}")
    check(e.px(sea_x, y) == water_row[1] and water_row[1] < 16, f"odd-x cell pixel ({sea_x},{y}) correct in VRAM: {e.px(sea_x, y)}")
    # the fleet list's icons start on odd x and end on even, so both of gfx_blit's edges are pinned
    for s in range(7):
        rows = b"".join(e.peek(VRAM + (PANEL_Y + 8 * s + r) * 240 + PANEL_X + ICON_DX, ICON_W) for r in range(ICON_H))
        if rows != e.art("art_icon", 5 * 2 * ICON_W * ICON_H)[(SHIP_LEN[s] - 1) * 2 * ICON_W * ICON_H:][:ICON_W * ICON_H]:
            check(False, f"the fleet list's icon {s} is the art, at odd x")
            break
    else: check(True, "the fleet list's icons are the art, at odd x")
    im = e.shot("6-battle-start")
    r, g, b = im.getpixel((grid_x * 2, y * 2))
    r2, g2, b2 = im.getpixel((sea_x * 2, y * 2))
    check((r, g, b) != (r2, g2, b2), f"and the screen shows them apart: {(r, g, b)} {(r2, g2, b2)}")

    # --- a shot at a known ship cell, with a cell of a ship that survives
    drawn = e.u32("cells_drawn")
    check((e.run(60) or e.u32("cells_drawn")) == drawn, "an idle minute of frames redraws no cell")
    hit = cells(own_o, 0)[0]
    e.goto(hit % 10, hit // 10)
    a0 = e.peek(OAM, 4)
    check(a0[0] == BIG[1] + 12 * (hit // 10) and (a0[2] | (a0[3] & 1) << 8) == BIG[0] + 12 * (hit % 10), f"the cursor sprite sits on the target: {a0.hex()}")
    drawn = e.u32("cells_drawn")
    im = e.shot("7-aim")
    calm = im.getpixel((470, 10))
    corner = im.getpixel(((BIG[0] + 12 * (hit % 10)) * 2, (BIG[1] + 12 * (hit // 10)) * 2))
    check(corner[0] > 200 and corner[1] > 200 and corner[2] < 60, f"and the cursor shows on screen: {corner}")
    e.run(2, A); e.run(4)
    flash = e.shot("8-hit-flash").getpixel((470, 10))
    check(sum(flash) > sum(calm) + 60, f"a hit flashes the screen: {calm} to {flash}")
    st = e.board("ob")[1]
    check(st[hit] == S_HIT, f"shot at a known ship cell is a hit: st {st[hit]}")
    check(e.u16("shots_fired") == 1 and e.u16("shots_hit") == 1, "counted as a shot and a hit")
    check(e.text("msg_text") == "HIT!", f"message {e.text('msg_text')!r}")
    check(e.peek(SND4ENV)[0] == 0xF3 and e.peek(SND4POLY)[0] == 0x65, "hit sound on the noise channel")
    check(e.u32("cells_drawn") - drawn == 1, f"a shot redraws one cell: {e.u32('cells_drawn') - drawn}")
    check(e.tiles(BIG)[hit] == T_HIT, "the hit cell is the hit mark on water, no hull")
    check(e.u32("frame_drops") == 0, "no frame dropped after the hit")

    # --- the enemy answers; its last shot is marked on your board, with no view flip
    pb_st = e.board("pb")[1]
    check(e.battle_ready(), "the enemy shoots and the turn comes back")
    pb_st2 = e.board("pb")[1]
    check(sum(pb_st2[i] != S_EMPTY for i in range(100)) == 1, "the enemy fired once")
    le = e.u8("last_enemy")
    check(pb_st2[le] != S_EMPTY and pb_st[le] == S_EMPTY, f"last_enemy names that cell: {le}")
    a1 = e.peek(OAM + 8, 4)
    check(a1[0] == SMALL[1] + 8 * (le // 10) and (a1[2] | (a1[3] & 1) << 8) == SMALL[0] + 8 * (le % 10), f"the marker sprite sits on it: {a1.hex()}")
    # on the screen, not just in OAM: its top-left corner is white, whatever the cell holds
    im = e.shot("8b-enemy-shot")
    corner = im.getpixel(((SMALL[0] + 8 * (le % 10)) * 2, (SMALL[1] + 8 * (le // 10)) * 2))
    check(corner == (255, 255, 255), f"and the marker shows on screen: {corner}")
    own_p = e.board("pb")[0]
    check(e.tiles(SMALL)[le] in (T_MISS, T_HULLHIT + seg(own_p, le), T_SUNK + seg(own_p, le)), "and it is drawn on your board")
    e.run(4)

    # --- a known water cell is a miss
    miss = water(own_o)[0]
    drawn = e.u32("cells_drawn")
    e.shoot(miss); e.run(1)
    st = e.board("ob")[1]
    check(st[miss] == S_MISS, f"shot at known water is a miss: st {st[miss]}")
    check(e.u16("shots_fired") == 2 and e.u16("shots_hit") == 1, "counted as a shot, not a hit")
    check(e.text("msg_text") == "MISS", f"message {e.text('msg_text')!r}")
    check(e.peek(SND4ENV)[0] == 0x21 and e.peek(SND4POLY)[0] == 0x71, "miss tick on the noise channel")
    check(e.tiles(BIG)[miss] == T_MISS, "the miss cell is the miss mark")
    e.shot("9-miss")
    check(e.battle_ready(), "turn comes back after the miss")

    # --- a spent square costs no turn, for a miss and for a hit
    for spent, what in ((miss, "miss"), (hit, "hit")):
        before = e.board("pb")[1], e.board("ob")[1], e.u16("shots_fired")
        e.shoot(spent); e.run(1)
        check(e.text("msg_text") == "ALREADY SHOT", f"re-shooting a {what} refused: {e.text('msg_text')!r}")
        check(e.state() == ST_BATTLE, "still your turn")
        e.run(40)
        after = e.board("pb")[1], e.board("ob")[1], e.u16("shots_fired")
        check(before == after, f"it costs no turn or stat ({what})")
        check(e.text("msg_text") == "", "and the refusal clears")

    # --- sinking: the whole ship redraws, the list marks it, the message names it
    sub = cells(own_o, 5)[0]
    row0 = e.panel_row(5)
    e.shoot(sub); e.run(1)
    check(e.text("msg_text") == "SUB SUNK", f"message {e.text('msg_text')!r}")
    check(e.peek(SND4ENV)[0] == 0xF7 and e.peek(SND4POLY)[0] == 0x76, "sunk sound on the noise channel")
    e.run(4)
    check(e.tiles(BIG)[sub] == T_SUNK + SEG_BOAT, "the sunk cell now shows its hull, wrecked")
    row = e.panel_row(5)
    check(C_RED in row and C_INK not in row and row != row0, "the fleet list marks it sunk")
    check(C_INK in e.panel_row(6), "and only that one")
    icons = e.art("art_icon", 5 * 2 * ICON_W * ICON_H)
    alive_ic, sunk_ic = icons[0:ICON_W * ICON_H], icons[ICON_W * ICON_H:2 * ICON_W * ICON_H]
    rows = b"".join(e.peek(VRAM + (PANEL_Y + 8 * 5 + r) * 240 + PANEL_X + ICON_DX, ICON_W) for r in range(ICON_H))
    check(rows == sunk_ic, "its icon is the sunk one")
    pips = lambda ic, c: {i for i, v in enumerate(ic) if v == c}
    check(pips(alive_ic, C_H0) != pips(sunk_ic, C_RED) and len(pips(sunk_ic, C_RED)) < len(pips(alive_ic, C_H0)), "whose mark is a different shape: an X, not a block")
    check(C_RED in e.peek(VRAM + (PANEL_Y + 8 * 5 + 3) * 240 + PANEL_X + 3, 6), "and the name is struck through")
    e.shot("10-sunk")
    check(e.battle_ready(), "turn comes back after the sinking")

    # --- known water: the cells around a sunk ship draw as water, on both boards, and are refused
    st = e.board("ob")[1]
    ring = [i for i in around(sub) if st[i] == S_EMPTY]
    check(len(ring) >= 2, f"unfired cells around the sunk sub: {ring}")
    got = e.tiles(BIG)
    check(all(got[i] == T_KNOWN for i in ring), "they draw as known water on the enemy board")
    check(got[miss] == T_MISS, "a fired miss still has its own mark")
    check(set(st) <= {S_EMPTY, S_MISS, S_HIT, S_SUNK} and all(st[i] == S_EMPTY for i in ring), "no new st[] value: they are still unfired")
    own_p, st_p, hits_p, alive_p = e.board("pb")
    st_p, hits_p = bytearray(st_p), bytearray(hits_p)
    psub = cells(own_p, 6)[0]
    if st_p[psub] != S_SUNK: alive_p -= 1
    st_p[psub], hits_p[6] = S_SUNK, 1
    e.poke_board("pb", own_p, st_p, hits_p, alive_p)
    e.run(4)
    pring = [i for i in around(psub) if st_p[i] == S_EMPTY]
    got = e.tiles(SMALL)
    check(len(pring) >= 2 and all(got[i] == T_KNOWN for i in pring), f"and on your board: {pring}")
    before = e.board("pb")[1], e.board("ob")[1], e.u16("shots_fired"), e.u16("shots_hit"), e.u8("last_enemy")
    e.shoot(ring[0]); e.run(1)
    check(e.text("msg_text") == "KNOWN WATER", f"firing there is refused: {e.text('msg_text')!r}")
    check(e.state() == ST_BATTLE, "still your turn")
    im = e.shot("10b-known-water")
    # (2,2) in a tile is the first dot of the known-water pattern
    known = im.getpixel(((BIG[0] + 12 * (ring[-1] % 10) + 2) * 2, (BIG[1] + 12 * (ring[-1] // 10) + 2) * 2))
    got = e.tiles(BIG)
    sea = next(i for i in range(100) if got[i] == T_WATER)
    plain = im.getpixel(((BIG[0] + 12 * (sea % 10) + 2) * 2, (BIG[1] + 12 * (sea // 10) + 2) * 2))
    check(known != plain, f"known water looks different from open sea on screen: {known} {plain}")
    e.run(40)
    after = e.board("pb")[1], e.board("ob")[1], e.u16("shots_fired"), e.u16("shots_hit"), e.u8("last_enemy")
    check(before == after, f"it costs no turn or stat: shots_fired still {after[2]}")
    check(e.text("msg_text") == "", "and the refusal clears")

    # --- win, set up by poke; both fleets revealed
    last = last_one_alive(e, "ob", False)
    e.run(4)
    e.goto(last % 10, last // 10)
    # A goes down on the last shot and stays down: the picture must hold its whole time regardless
    frames = watch(e, lambda: e.state() == ST_SPLASH, A)
    check(e.state() == ST_SPLASH, "the last enemy ship sunk brings up the win picture")
    check_fade("battle to win picture", None, frames)
    e.run(1, A)
    check(e.until(lambda: e.reg(SNDSTAT) & 1, 10, A), "the win jingle sounds on the square channel")
    check(e.peek(SND1ENV)[0] == 0x73, "with the GB's envelope")
    check(e.fb() == e.art("art_win", 38400), "the screen is the win art, pixel for pixel")
    check(e.pal(ART_PAL_FIRST, ART_PAL_N) == e.art("art_win_pal", ART_PAL_N * 2), "in its palette")
    shared_range_kept(e, "win picture")
    e.shot("11a-win-art")
    f0 = e.u32("frames")
    e.run(SPLASH_FRAMES - 30, A)
    check(e.state() == ST_SPLASH, "a button held since the last shot does not skip it")
    start = e.grab()
    frames = watch(e, lambda: e.state() == ST_END, A)
    held = e.u32("frames") - f0
    check(e.state() == ST_END and SPLASH_FRAMES - 10 <= held <= SPLASH_FRAMES + 45, f"it holds about 5 seconds, then the end screen: {held} frames to the end screen, fades and painting included")
    check_fade("win picture to end screen", start, frames)
    shared_range_kept(e, "end screen")
    e.run(120, A)
    check(e.state() == ST_END, "and A still held does not touch the end screen")
    e.run(10)
    chrome_clear_of_sea(e.fb(), "end screen")
    check(e.text("status_text") == "YOU WIN!", f"status {e.text('status_text')!r}")
    fired, hits_n = e.u16("shots_fired"), e.u16("shots_hit")
    m = e.text("msg_text")
    check(m == f"{fired} SHOTS {hits_n * 100 // fired}%", f"shots and accuracy: {m!r}")
    pc = sum(e.px(12 + 6 * (len(m) - 1) + x, 145 + y) == C_YEL for x in range(5) for y in range(7))
    check(pc > 0, f"and the % is drawn ({pc} pixels)")
    own_o, st_o, _, _ = e.board("ob")
    own_p, st_p, _, _ = e.board("pb")
    got = e.tiles(BIG)
    check(got == want_tiles(own_o, st_o, "end"), "enemy fleet fully revealed, as hulls")
    check(e.tiles(SMALL) == want_tiles(own_p, st_p, "end"), "your fleet fully revealed")
    check(got[miss] == T_WATER, "the end screen hides misses, as on GB")
    check(T_KNOWN not in got, "and known water with them")
    check(e.peek(OAM + 1)[0] & 3 == 2 and e.peek(OAM + 9)[0] & 3 == 2, "cursor and marker hidden")
    e.shot("11-win")
    e.tap(A); e.tap(SELECT); e.run(5)
    check(e.state() == ST_END, "the end screen holds until START: no view flip")
    check(e.u32("frame_drops") == 0, f"no frame dropped through the first game ({e.u32('frame_drops')})")

    # --- every ship length both ways round, on both boards. Ids 0-8, on purpose past NSHIP: this is
    #     the renderer's rule alone. Two ships side by side must not merge: 2 and 3 lie against each other.
    gallery = [EMPTY] * 100
    for id, (x, y, n, horiz) in enumerate(((0, 0, 5, 1), (9, 0, 5, 0), (0, 2, 4, 1), (0, 3, 4, 1), (7, 2, 4, 0), (0, 5, 3, 1), (5, 5, 3, 0), (0, 7, 2, 1), (3, 7, 2, 0))):
        for k in range(n): gallery[y * 10 + x + k if horiz else (y + k) * 10 + x] = id
    gallery[99], gallery[72] = 9, 10            # the one-cell boats, one nose to nose with a hull
    keep = e.board("ob"), e.board("pb")
    no_shots = bytes(100)
    for name in ("ob", "pb"): e.poke_board(name, gallery, no_shots, bytes(7), 7)
    e.run(6)
    got = e.tiles(BIG), e.tiles(SMALL)
    want = want_tiles(gallery, no_shots, "end")
    check(got[0] == want and got[1] == want, "every hull length both ways round draws its segments, big and small")
    check({T_HULL + g for g in range(7)} <= set(got[0]), "all seven segments are in that")
    check(got[0][20] == T_HULL + SEG_H_STERN and got[0][30] == T_HULL + SEG_H_STERN, "two ships side by side stay two ships")
    check(got[0][71] == T_HULL + SEG_H_BOW and got[0][72] == T_HULL + SEG_BOAT, "and so do two end to end")
    check(e.u32("frame_drops") == 0, "no frame dropped drawing them")
    e.shot("16-all-hulls")
    # the same fleet damaged: every other cell hit, every third sunk
    states = bytes(S_SUNK if i % 3 == 0 else S_HIT if i % 3 == 1 else S_EMPTY for i in range(100))
    for name in ("ob", "pb"): e.poke_board(name, gallery, states, bytes(7), 7)
    e.run(6)
    want = want_tiles(gallery, states, "end")
    check(e.tiles(BIG) == want and e.tiles(SMALL) == want, "and hit and sunk hulls in every segment")
    e.shot("16b-hull-states")
    for name, b in zip(("ob", "pb"), keep): e.poke_board(name, *b)
    e.run(6)

    # --- START replays without the title, cleanly, and deals another fleet
    start = e.grab()
    e.run(1, START)
    frames = watch(e, lambda: e.state() == ST_MENU, START)
    check(e.state() == ST_MENU, f"START goes to the menu, not the title: state {e.state()}")
    check_fade("end screen to menu", start, frames)
    shared_range_kept(e, "menu again")
    e.run(5)
    check(e.px(START_X, START_Y) == C_BG, "no PRESS START")
    e.tap(SELECT)                               # NORMAL to HARD
    check(e.u8("ai_level") == 2, "HARD")
    e.run(7)                                    # a different moment from the first game
    prev = own_o
    check(enter_battle(e), "replay: A deals a random game")
    plain_deal_check(e, "HARD")
    e.run(10)
    own_o, st_o, hits_o, alive_o = e.board("ob")
    check(own_o != prev, "two games deal different fleets")
    check(legal_fleet(own_o) and set(st_o) == {S_EMPTY} and set(hits_o) == {0} and alive_o == 7, "a clean enemy board")
    check(e.tiles(BIG) == [T_WATER] * 100, "nothing left on the big board")
    own_p, st_p, _, alive_p = e.board("pb")
    check(legal_fleet(own_p) and set(st_p) == {S_EMPTY} and alive_p == 7, "a clean board of yours")
    check(e.tiles(SMALL) == want_tiles(own_p, st_p, "ships"), "and it is drawn clean")
    check(e.u16("shots_fired") == 0 and e.u16("shots_hit") == 0, "stats reset")
    check(e.peek(OAM + 9)[0] & 3 == 2, "no enemy-shot marker yet")
    check(all(C_RED not in e.panel_row(s) for s in range(7)), "fleet list shows nothing sunk")
    e.shot("12-replay")
    # how much a frame costs now that a cell copies art
    e.poke(e.sym["worst_frame"], bytes(4))
    e.run(60)
    idle = e.u32("worst_frame")
    print(f"     idle board scan: {idle} of {FRAME_CYCLES} cycles ({idle * 100 // FRAME_CYCLES}%)")
    check(idle < FRAME_CYCLES, "it fits a frame")
    e.poke(e.sym["worst_frame"], bytes(4))
    e.redraw(BIG); e.redraw(SMALL)
    painted = e.u32("frames"), e.u32("cells_drawn")
    e.until(lambda: e.u32("cells_drawn") - painted[1] >= 200, 30)
    cost = e.u32("worst_frame")
    print(f"     repainting both boards: {e.u32('frames') - painted[0]} frames, the worst {cost} of {FRAME_CYCLES} cycles ({cost * 100 // FRAME_CYCLES}%)")
    check(e.u32("cells_drawn") - painted[1] == 200 and cost < FRAME_CYCLES, "it paints the 200 cells inside the budget")
    check(e.u32("frame_drops") == 0, f"with no frame dropped ({e.u32('frame_drops')})")

    # --- a whole real game at HARD, won by shooting known cells. The first enemy shot is on an
    #     empty board, the slowest case for ai_prob_pick.
    ai0 = e.u32("ai_cycles")
    order = sorted((i for i in range(100) if own_o[i] != EMPTY), key=lambda i: own_o[i])
    for i in order:
        check_ready = e.battle_ready()
        if not check_ready: break
        e.shoot(i)
    check(e.wait_state(ST_SPLASH), "a whole game at HARD plays through to the win")
    # a fresh A inside the lock does nothing; a fresh A after it skips, and does not carry on to the end screen
    win_art = e.art("art_win", 38400)
    e.tap(A); e.run(20)
    # the screen, not state: state stays ST_SPLASH until the end screen is up, through a skip's fade
    check(e.state() == ST_SPLASH and e.fb() == win_art and not is_black(e.grab()), "a fresh A while the jingle plays does not skip the picture")
    e.run(SPLASH_LOCK)
    f0 = e.u32("frames")
    e.run(2, A)
    check(e.wait_state(ST_END, 40) and e.u32("frames") - f0 < 40, f"a fresh A after it does: {e.u32('frames') - f0} frames to the end screen")
    check(e.u16("shots_fired") == 18 and e.u16("shots_hit") == 18, f"18 shots, 18 hits: {e.u16('shots_fired')}, {e.u16('shots_hit')}")
    cost = e.u32("ai_cycles")
    print(f"     ai_next at HARD on an empty board: {cost} cycles, {cost / FRAME_CYCLES:.1f} frames (Thumb from ROM)")
    check(cost > ai0, "the HARD hunt was measured")
    check(cost < THINK_FRAMES * FRAME_CYCLES, f"and it fits the enemy's {THINK_FRAMES}-frame pause")
    check(e.u32("frame_drops") == 0, f"no frame dropped through a HARD game ({e.u32('frame_drops')})")
    e.run(10)
    e.shot("13-win-hard")

    # --- a whole game at ADMIRAL, lost on purpose so it fires until your whole fleet is gone
    e.tap(START); check(e.wait_state(ST_MENU, 40), "START goes to the menu")
    e.tap(SELECT)
    check(e.u8("ai_level") == 3, "ADMIRAL")
    # through the real menu, a frame at a time: a deal a frame, each within its frame
    e.poke(e.sym["worst_frame"], bytes(4))
    drops0, f0 = e.u32("frame_drops"), e.u32("frames")
    e.run(2, A)
    check(e.until(lambda: e.state() == ST_BATTLE, 20), "an ADMIRAL game")
    deal_frames = e.u32("frames") - f0
    e.run(1)                                    # the frame that dealt your fleet has ended
    cost = e.u32("worst_frame")
    check(deal_frames >= 5, f"it took the 4 deals a frame each, after the key: {deal_frames} frames")
    print(f"     dealing ADMIRAL's fleet: {deal_frames} frames from the key, the worst {cost} of {FRAME_CYCLES} cycles ({cost * 100 // FRAME_CYCLES}%)")
    check(cost < FRAME_CYCLES and e.u32("frame_drops") == drops0, f"and no frame dropped dealing it ({e.u32('frame_drops') - drops0})")
    check(e.battle_ready(), "and the first turn comes")
    clever_deal_check(e, "ADMIRAL")
    first_admiral = e.board("ob")[0]
    e.run(60)                                   # let the boards paint in
    e.poke(e.sym["ai_cycles"], bytes(4))        # HARD's stall is not ADMIRAL's
    rows = admiral_game(e, (25, "13b-admiral-midgame"))
    if rows is None:
        check(False, f"a whole game at ADMIRAL plays through, each shot inside the pause ({e.u32('frame_drops')} frames dropped)")
        return 1
    check(e.wait_state(ST_END), "and ends")
    e.run(10)
    check(e.text("status_text") == "YOU LOSE" and e.board("pb")[3] == 0, f"ADMIRAL sank the fleet in {len(rows)} shots")
    e.shot("13c-admiral-win")
    check(all(r["think"] == THINK_FRAMES for r in rows), f"every shot took the {THINK_FRAMES}-frame pause: {sorted({r['think'] for r in rows})}")
    check(not any(r["spent"] for r in rows), "it never fired at a spent cell")
    check(not any(r["water"] for r in rows), "and never at known water")
    def spread(what, rs):
        s = [r["samples"] for r in rs]
        print(f"     samples per turn, {what}: min {min(s)} mean {sum(s) / len(s):.0f} max {max(s)} ({len(s)} turns)")
    spread("whole game", rows); spread("first 10", rows[:10]); spread("last 10", rows[-10:])
    # the fallback is legal, and rare: 1 turn in about 3000 at the ROM's rate
    check(sum(r["samples"] < 50 for r in rows) <= 2, f"sampling carried the game: {sum(r['samples'] < 50 for r in rows)} fallback turns")
    cost = max(r["cost"] for r in rows)
    print(f"     worst frame of an ADMIRAL turn: {cost} of {FRAME_CYCLES} cycles ({cost * 100 // FRAME_CYCLES}%)")
    print(f"     mc_pick at most {e.u32('ai_cycles')} cycles")
    check(cost < FRAME_CYCLES, "every sampling frame fits a frame")

    # --- ADMIRAL with no valid fleet: the carrier exists nowhere (a tight manual layout can leave a
    #     ship unplaced) and all water is fired, so the fallback must carry a whole game
    e.tap(START); check(e.wait_state(ST_MENU, 40), "START goes to the menu")
    for _ in range(4): e.tap(SELECT)
    check(e.u8("ai_level") == 3, "ADMIRAL again")
    check(enter_battle(e), "an ADMIRAL game with no valid fleet")
    clever_deal_check(e, "ADMIRAL again")
    check(e.board("ob")[0] != first_admiral, "and it is not the fleet of the last ADMIRAL game")
    e.run(60)
    own_p, st_p, hits_p, _ = e.board("pb")
    own_p = bytearray(EMPTY if c == 0 else c for c in own_p)
    st_p = bytearray(S_MISS if c == EMPTY else S_EMPTY for c in own_p)
    bs = cells(own_p, 1)[1]
    st_p[bs] = S_HIT
    hits_p = bytearray(7); hits_p[1] = 1
    e.poke_board("pb", own_p, st_p, hits_p, 6)
    rows = admiral_game(e, (1, "13d-admiral-fallback"))
    if rows is None:
        check(False, "the fallback plays a whole game through, each shot inside the pause")
        return 1
    check(e.wait_state(ST_END), "and it ends")
    check(all(r["samples"] < 50 for r in rows), f"sampling got too few fleets on every turn: {[r['samples'] for r in rows][:5]}")
    print(f"     tries in that budget, first turn: {rows[0]['tries']}; worst frame {max(r['cost'] for r in rows)}; pick at most {e.u32('ai_cycles')} cycles")
    check(all(r["think"] == THINK_FRAMES and not r["spent"] and not r["water"] for r in rows), "every fallback shot legal and on time")
    check(max(r["cost"] for r in rows) < FRAME_CYCLES, "and within a frame")
    e.run(10)

    # --- a loss, set up by poke, at EASY: only one cell left to fire at, and it is your last ship
    e.tap(START); check(e.wait_state(ST_MENU, 40), "START goes to the menu")
    e.tap(SELECT)
    check(e.u8("ai_level") == 0, "EASY")
    check(enter_battle(e), "third game")
    plain_deal_check(e, "EASY")
    own_o = e.board("ob")[0]
    last = last_one_alive(e, "pb", True)
    e.run(4)
    e.shot("14-before-loss")
    i = water(own_o)[0]
    e.goto(i % 10, i // 10)
    e.poke(e.sym["worst_frame"], bytes(4))
    e.tap(A)
    frames = watch(e, lambda: e.state() == ST_SPLASH, START, 200)   # START down from before the picture
    check(e.state() == ST_SPLASH, "the enemy sinking your last ship brings up the lose picture")
    check_fade("battle to lose picture", None, frames)
    e.run(1, START)
    check(e.fb() == e.art("art_lose", 38400), "the screen is the lose art, pixel for pixel")
    check(e.pal(ART_PAL_FIRST, ART_PAL_N) == e.art("art_lose_pal", ART_PAL_N * 2), "in its palette")
    shared_range_kept(e, "lose picture")
    e.shot("15a-lose-art")
    e.run(SPLASH_LOCK + 30, START)
    check(e.state() == ST_SPLASH, "START held from before the picture does not skip it")
    e.run(2)
    start = e.grab()
    e.run(1, START)
    frames = watch(e, lambda: e.state() == ST_END, START)
    check(e.state() == ST_END and len(frames) < 40, f"a fresh START skips it: {len(frames)} frames to the end screen")
    check_fade("lose picture to end screen", start, frames)
    e.run(60, START)
    check(e.state() == ST_END, "and that same START, still held, does not leave the end screen")
    e.run(10)
    check(e.text("status_text") == "YOU LOSE", f"status {e.text('status_text')!r}")
    check(e.board("pb")[3] == 0 and e.board("ob")[3] == 7, "your fleet gone, theirs afloat")
    own_o = e.board("ob")[0]
    check(e.tiles(BIG) == want_tiles(own_o, e.board("ob")[1], "end"), "enemy fleet revealed on a loss")
    check(e.reg(SNDSTAT) & 0x80, "sound still on")
    e.shot("15-lose")
    cost = e.u32("worst_frame")
    print(f"     worst frame from the last shot to the end screen, loading the picture included: {cost} of {FRAME_CYCLES} cycles ({cost * 100 // FRAME_CYCLES}%)")
    check(cost < FRAME_CYCLES, "it fits a frame")
    e.tap(START)
    check(e.wait_state(ST_MENU, 40), "START replays after a loss")

    # --- NORMAL's hunt and one more turn, so every level has fired a shot
    e.tap(SELECT)
    check(e.u8("ai_level") == 1, "NORMAL")
    check(enter_battle(e), "fourth game")
    plain_deal_check(e, "NORMAL")
    own_o = e.board("ob")[0]
    e.shoot(water(own_o)[0])
    check(e.battle_ready(), "the turn comes back at NORMAL")
    check(sum(s != S_EMPTY for s in e.board("pb")[1]) == 1, "and the enemy fired once")

    # --- hidden information. The same shots over three different own[]: the real fleet, another, and
    #     no ships at all. Every ship hit but for one cell each, so no ship is sunk. It must be the
    #     same pixels: VRAM and screen, each repainted from nothing, or shown[] would hide a leak.
    own_o, _, _, _ = e.board("ob")
    st = bytearray(100)
    for s in range(5):
        for i in cells(own_o, s)[:-1]: st[i] = S_HIT
    for i in water(own_o)[::3]: st[i] = S_MISS
    other = bytes((own_o[(i + 37) % 100]) for i in range(100))
    views = []
    for what, own in (("with its ships", own_o), ("with other ships", other), ("with none", bytes([EMPTY] * 100))):
        # hits[] follows own[]: a partial count can never sink anything
        hits = [sum(st[i] == S_HIT and own[i] == s for i in range(100)) for s in range(7)]
        e.poke_board("ob", own, st, hits, 7)
        e.redraw(BIG)
        e.run(6)
        im = e.grab().crop((BIG[0], BIG[1], BIG[0] + 121, BIG[1] + 121))
        views.append((what, e.tiles(BIG), im.tobytes()))
    check(all(v[1] == views[2][1] for v in views), "an unsunk enemy board draws the same cells, ships or none, in VRAM")
    check(all(v[2] == views[2][2] for v in views), "and the same pixels on the screen")
    check(views[0][1] == [T_HIT if st[i] == S_HIT else T_MISS if st[i] == S_MISS else T_WATER for i in range(100)], "hits are the only trace: hit marks, misses, and water")
    e.shot("17-hidden-check")
    check(e.u32("frame_drops") == 0, f"no frame dropped after boot, all games ({e.u32('frame_drops')})")
    e.p.stdin.close(); e.p.wait()

    # --- the seed: two power-ons that differ only in when the keys were pressed
    fleets = []
    for wait in (0, 13):
        e = Emu("navalhunter.gba")
        e.run(10); e.tap(START); e.wait_state(ST_MENU, 40); e.run(5 + wait)
        e.tap(A); e.run(20)
        fleets.append(e.board("ob")[0] + e.board("pb")[0])
        e.p.stdin.close(); e.p.wait()
    check(fleets[0] != fleets[1], "power-ons with different key timing deal different fleets")
    check(fleets[0][:100] != fleets[0][100:], "and the two fleets of one game differ")

    print("FAILED" if fails else "all good", f"({fails} failures)")
    return 1 if fails else 0

sys.exit(main())
