#!/usr/bin/env python3
"""Drives the real ROM in libmgba (tools/emu.c) with real key presses. What it pins is what
only the ROM can show. Boards are set up by poke, never by a test build. Screenshots go to
shots/. Exit 1 on any FAIL."""
import os, subprocess, sys
from PIL import Image

DISPCNT, VRAM, OAM = 0x04000000, 0x06000000, 0x07000000
SND4ENV, SND4POLY, SNDSTAT, SND1ENV = 0x04000079, 0x0400007C, 0x04000084, 0x04000063
ST_BOOT, ST_TITLE, ST_MENU, ST_PLACE, ST_BATTLE, ST_ENEMY, ST_END = range(7)
C_BG, C_INK, C_GRID, C_SEA, C_SHIP, C_BAD, C_RED, C_DKRED, C_YEL, C_DIM, C_KNOWN = range(11)  # main.c's palette
A, B, SELECT, START, RIGHT, LEFT, UP, DOWN = 1, 2, 4, 8, 16, 32, 64, 128
S_EMPTY, S_MISS, S_HIT, S_SUNK = range(4)
EMPTY = 255
SHIP_LEN = [5, 4, 3, 2, 2, 1, 1]
BIG, SMALL = (12, 22, 12), (144, 12, 8)
PANEL_X, PANEL_Y = 144, 103
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
    # a pane's cell (x,y): the top-left pixel of its inside, past the grid line
    def cell_px(self, pane, i, dx=0, dy=0):
        px, py, s = pane
        return self.px(px + i % 10 * s + 1 + dx, py + i // 10 * s + 1 + dy)
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
    n = ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"]
    got = ["".join("#" if e.px(84 + x, 40 + y) == C_INK else "." for x in range(5)) for y in range(7)]
    check(got == n, f"text draws the font's N of the title: {got}")
    e.shot("1-title")
    for key in (A, B, SELECT, RIGHT, LEFT, UP, DOWN):
        e.tap(key)
    e.run(60)
    check(e.state() == ST_TITLE, "title holds through A, B, SELECT and the d-pad")
    seen = set()
    for _ in range(130):
        seen.add(e.px(87, 100)); e.run()
    check(seen == {C_BG, C_YEL}, f"PRESS START blinks: colours {sorted(seen)}")
    check(e.u32("frame_drops") == 0, f"no frame dropped on the title ({e.u32('frame_drops')})")
    f0 = e.u32("frames")
    e.run(300)
    check(e.u32("frames") - f0 == 300, f"VBlank IRQ ran for all 300 frames ({e.u32('frames') - f0})")
    e.tap(START)
    check(e.state() == ST_MENU, f"START leaves the title: state {e.state()}")

    # --- menu: SELECT cycles the level, other keys do nothing
    e.run(5)
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
    check(e.cell_px(BIG, 0) == C_SHIP and e.cell_px(BIG, 4) == C_SHIP and e.cell_px(BIG, 5) == C_SEA, "carrier preview shown legal at A1")
    e.shot("3-place-start")
    e.tap(A)                                    # carrier along the top row
    e.run(5)
    own = e.board("pb")[0]
    check(cells(own, 0) == [0, 1, 2, 3, 4] and e.u8("place_s") == 1, f"carrier committed: {cells(own, 0)}")
    check(set(e.panel_row(0)) != {C_BG} and set(e.panel_row(1)) == {C_BG}, "fleet list shows only the ship placed")

    # refused: the battleship's preview sits on the carrier
    check(e.cell_px(BIG, 0) == C_BAD and e.cell_px(BIG, 3) == C_BAD, "illegal position shown in the refused colour")
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
    check(e.cell_px(BIG, 10) == C_BAD and e.cell_px(BIG, 40) == C_BAD, "a touching position is refused too (row 2)")
    e.tap(A); e.run(31)
    check(e.u8("place_s") == 1, "touching position not committed")
    e.tap(DOWN); e.run(2)
    check(e.cell_px(BIG, 20) == C_SHIP and e.cell_px(BIG, 50) == C_SHIP, "vertical preview legal at A3")
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
    check(all(e.cell_px(BIG, i) == C_SEA for i in range(100)), "enemy ships hidden: the big board is all water")
    check(all(e.cell_px(SMALL, i) == (C_SHIP if own[i] != EMPTY else C_SEA) for i in range(100)), "your ships always shown")
    check(all(set(e.panel_row(s)) != {C_BG} for s in range(7)), "enemy fleet list full in battle")

    # Phase 1's bug: a byte write lands in both bytes of a halfword. Pixels the board draws:
    # the grid line (even x) drawn first, then the cell (odd x) beside it.
    grid_x, sea_x = BIG[0] + 12 * 5, BIG[0] + 12 * 5 + 1      # clear of the cursor at A1
    y = BIG[1] + 5
    check(e.px(grid_x, y) == C_GRID, f"even-x grid pixel ({grid_x},{y}) correct in VRAM: {e.px(grid_x, y)}")
    check(e.px(sea_x, y) == C_SEA, f"odd-x cell pixel ({sea_x},{y}) correct in VRAM: {e.px(sea_x, y)}")
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
    check(e.cell_px(BIG, hit) == C_RED, "the hit cell is drawn")
    # the 5px mark ends on even x: even written after its odd neighbour
    check(e.cell_px(BIG, hit, 7, 5) == C_YEL and e.cell_px(BIG, hit, 8, 5) == C_RED, "even-x pixel written after odd-x neighbour survives")
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
    check(e.cell_px(SMALL, le) in (C_SEA, C_RED, C_DKRED), "and it is drawn on your board")
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
    # the 3px mark starts on odd x: odd written after its even neighbour
    check(e.cell_px(BIG, miss, 3, 5) == C_SEA and e.cell_px(BIG, miss, 4, 5) == C_INK, "odd-x pixel written after even-x neighbour survives")
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
    check(e.cell_px(BIG, sub) == C_DKRED, "the sunk cell is drawn")
    row = e.panel_row(5)
    check(C_RED in row and C_INK not in row and row != row0, "the fleet list marks it sunk")
    check(C_INK in e.panel_row(6), "and only that one")
    e.shot("10-sunk")
    check(e.battle_ready(), "turn comes back after the sinking")

    # --- known water: the cells around a sunk ship draw as water, on both boards, and are refused
    st = e.board("ob")[1]
    ring = [i for i in around(sub) if st[i] == S_EMPTY]
    check(len(ring) >= 2, f"unfired cells around the sunk sub: {ring}")
    check(all(e.cell_px(BIG, i) == C_KNOWN for i in ring), "they draw as known water on the enemy board")
    check(all(e.cell_px(BIG, i, 5, 5) == C_KNOWN for i in ring), "with no mark in the middle, unlike a fired miss")
    check(e.cell_px(BIG, miss, 5, 5) == C_INK, "a fired miss still has its mark")
    check(set(st) <= {S_EMPTY, S_MISS, S_HIT, S_SUNK} and all(st[i] == S_EMPTY for i in ring), "no new st[] value: they are still unfired")
    own_p, st_p, hits_p, alive_p = e.board("pb")
    st_p, hits_p = bytearray(st_p), bytearray(hits_p)
    psub = cells(own_p, 6)[0]
    if st_p[psub] != S_SUNK: alive_p -= 1
    st_p[psub], hits_p[6] = S_SUNK, 1
    e.poke_board("pb", own_p, st_p, hits_p, alive_p)
    e.run(4)
    pring = [i for i in around(psub) if st_p[i] == S_EMPTY]
    check(len(pring) >= 2 and all(e.cell_px(SMALL, i) == C_KNOWN for i in pring), f"and on your board: {pring}")
    before = e.board("pb")[1], e.board("ob")[1], e.u16("shots_fired"), e.u16("shots_hit"), e.u8("last_enemy")
    e.shoot(ring[0]); e.run(1)
    check(e.text("msg_text") == "KNOWN WATER", f"firing there is refused: {e.text('msg_text')!r}")
    check(e.state() == ST_BATTLE, "still your turn")
    im = e.shot("10b-known-water")
    known = im.getpixel(((BIG[0] + 12 * (ring[-1] % 10) + 3) * 2, (BIG[1] + 12 * (ring[-1] // 10) + 3) * 2))
    sea = next(i for i in range(100) if st[i] == S_EMPTY and i not in ring and e.cell_px(BIG, i) == C_SEA)
    plain = im.getpixel(((BIG[0] + 12 * (sea % 10) + 3) * 2, (BIG[1] + 12 * (sea // 10) + 3) * 2))
    check(known != plain, f"known water looks different from open sea on screen: {known} {plain}")
    e.run(40)
    after = e.board("pb")[1], e.board("ob")[1], e.u16("shots_fired"), e.u16("shots_hit"), e.u8("last_enemy")
    check(before == after, f"it costs no turn or stat: shots_fired still {after[2]}")
    check(e.text("msg_text") == "", "and the refusal clears")

    # --- win, set up by poke; both fleets revealed
    last = last_one_alive(e, "ob", False)
    e.run(4)
    e.shoot(last)
    check(e.until(lambda: e.reg(SNDSTAT) & 1, 30), "the win jingle sounds on the square channel")
    check(e.peek(SND1ENV)[0] == 0x73, "with the GB's envelope")
    check(e.wait_state(ST_END, 200), "the last enemy ship sunk ends the game")
    e.run(10)
    check(e.text("status_text") == "YOU WIN!", f"status {e.text('status_text')!r}")
    fired, hits_n = e.u16("shots_fired"), e.u16("shots_hit")
    m = e.text("msg_text")
    check(m == f"{fired} SHOTS {hits_n * 100 // fired}%", f"shots and accuracy: {m!r}")
    pc = sum(e.px(12 + 6 * (len(m) - 1) + x, 145 + y) == C_YEL for x in range(5) for y in range(7))
    check(pc > 0, f"and the % is drawn ({pc} pixels)")
    own_o, st_o, _, _ = e.board("ob")
    own_p, st_p, _, _ = e.board("pb")
    check(all(e.cell_px(BIG, i) != C_SEA for i in range(100) if own_o[i] != EMPTY), "enemy fleet fully revealed")
    check(all(e.cell_px(SMALL, i) != C_SEA for i in range(100) if own_p[i] != EMPTY), "your fleet fully revealed")
    check(e.cell_px(BIG, miss) == C_SEA, "the end screen hides misses, as on GB")
    check(C_KNOWN not in {e.cell_px(BIG, i) for i in range(100)}, "and known water with them")
    check(e.peek(OAM + 1)[0] & 3 == 2 and e.peek(OAM + 9)[0] & 3 == 2, "cursor and marker hidden")
    e.shot("11-win")
    e.tap(A); e.tap(SELECT); e.run(5)
    check(e.state() == ST_END, "the end screen holds until START: no view flip")
    check(e.u32("frame_drops") == 0, f"no frame dropped through the first game ({e.u32('frame_drops')})")

    # --- START replays without the title, cleanly, and deals another fleet
    e.tap(START)
    check(e.state() == ST_MENU, f"START goes to the menu, not the title: state {e.state()}")
    e.run(5)
    check(e.px(87, 100) != C_YEL, "no PRESS START")
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
    check(all(e.cell_px(BIG, i) == C_SEA for i in range(100)), "nothing left on the big board")
    own_p, st_p, _, alive_p = e.board("pb")
    check(legal_fleet(own_p) and set(st_p) == {S_EMPTY} and alive_p == 7, "a clean board of yours")
    check(all(e.cell_px(SMALL, i) == (C_SHIP if own_p[i] != EMPTY else C_SEA) for i in range(100)), "and it is drawn clean")
    check(e.u16("shots_fired") == 0 and e.u16("shots_hit") == 0, "stats reset")
    check(e.peek(OAM + 9)[0] & 3 == 2, "no enemy-shot marker yet")
    check(all(C_RED not in e.panel_row(s) for s in range(7)), "fleet list shows nothing sunk")
    e.shot("12-replay")

    # --- a whole real game at HARD, won by shooting known cells. The first enemy shot is on an
    #     empty board, the slowest case for ai_prob_pick.
    ai0 = e.u32("ai_cycles")
    order = sorted((i for i in range(100) if own_o[i] != EMPTY), key=lambda i: own_o[i])
    for i in order:
        check_ready = e.battle_ready()
        if not check_ready: break
        e.shoot(i)
    check(e.wait_state(ST_END), "a whole game at HARD plays through to the win")
    check(e.u16("shots_fired") == 18 and e.u16("shots_hit") == 18, f"18 shots, 18 hits: {e.u16('shots_fired')}, {e.u16('shots_hit')}")
    cost = e.u32("ai_cycles")
    print(f"     ai_next at HARD on an empty board: {cost} cycles, {cost / FRAME_CYCLES:.1f} frames (Thumb from ROM)")
    check(cost > ai0, "the HARD hunt was measured")
    check(cost < THINK_FRAMES * FRAME_CYCLES, f"and it fits the enemy's {THINK_FRAMES}-frame pause")
    check(e.u32("frame_drops") == 0, f"no frame dropped through a HARD game ({e.u32('frame_drops')})")
    e.run(10)
    e.shot("13-win-hard")

    # --- a whole game at ADMIRAL, lost on purpose so it fires until your whole fleet is gone
    e.tap(START); e.run(5)
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
    e.tap(START); e.run(5)
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
    e.tap(START); e.run(5)
    e.tap(SELECT)
    check(e.u8("ai_level") == 0, "EASY")
    check(enter_battle(e), "third game")
    plain_deal_check(e, "EASY")
    own_o = e.board("ob")[0]
    last = last_one_alive(e, "pb", True)
    e.run(4)
    e.shot("14-before-loss")
    e.shoot(water(own_o)[0])
    check(e.wait_state(ST_END), "the enemy sinking your last ship ends the game")
    e.run(10)
    check(e.text("status_text") == "YOU LOSE", f"status {e.text('status_text')!r}")
    check(e.board("pb")[3] == 0 and e.board("ob")[3] == 7, "your fleet gone, theirs afloat")
    own_o = e.board("ob")[0]
    check(all(e.cell_px(BIG, i) == C_SHIP for i in range(100) if own_o[i] != EMPTY), "enemy fleet revealed on a loss")
    check(e.reg(SNDSTAT) & 0x80, "sound still on")
    e.shot("15-lose")
    e.tap(START); e.run(5)
    check(e.state() == ST_MENU, "START replays after a loss")

    # --- NORMAL's hunt and one more turn, so every level has fired a shot
    e.tap(SELECT)
    check(e.u8("ai_level") == 1, "NORMAL")
    check(enter_battle(e), "fourth game")
    plain_deal_check(e, "NORMAL")
    own_o = e.board("ob")[0]
    e.shoot(water(own_o)[0])
    check(e.battle_ready(), "the turn comes back at NORMAL")
    check(sum(s != S_EMPTY for s in e.board("pb")[1]) == 1, "and the enemy fired once")
    check(e.u32("frame_drops") == 0, f"no frame dropped after boot, all games ({e.u32('frame_drops')})")
    e.p.stdin.close(); e.p.wait()

    # --- the seed: two power-ons that differ only in when the keys were pressed
    fleets = []
    for wait in (0, 13):
        e = Emu("navalhunter.gba")
        e.run(10); e.tap(START); e.run(5 + wait)
        e.tap(A); e.run(20)
        fleets.append(e.board("ob")[0] + e.board("pb")[0])
        e.p.stdin.close(); e.p.wait()
    check(fleets[0] != fleets[1], "power-ons with different key timing deal different fleets")
    check(fleets[0][:100] != fleets[0][100:], "and the two fleets of one game differ")

    print("FAILED" if fails else "all good", f"({fails} failures)")
    return 1 if fails else 0

sys.exit(main())
