#!/usr/bin/env python3
"""Images for docs/PLAYER_GUIDE.md and .html, in docs/img.  make guide
Plays the real ROM with real keys, so every screen is one the game really shows, and takes the cell
art and fleet icons from the ROM too. The player is a bot that peeks at the enemy's ships now and
then, so that a game comes out believable, and it wins or loses to order."""
import os, random, sys
from PIL import Image
sys.path.insert(0, "tools"); sys.dont_write_bytecode = True
from e2e import (Emu, A, B, SELECT, START, DOWN, RIGHT, ST_TITLE, ST_MENU, ST_PLACE, ST_BATTLE, ST_END, ST_SPLASH,
                 S_EMPTY, S_HIT, S_SUNK, EMPTY, T_WATER, T_MISS, T_HIT, T_KNOWN, T_HULLHIT, T_SUNK, SEG_H_MID,
                 T_COUNT, ICON_W, ICON_H)

OUT = "docs/img/"

def snap(e, name):
    e.grab().resize((480, 320), Image.NEAREST).save(OUT + name + ".png")

def around(i, cross=False):
    x, y = i % 10, i // 10
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if (dx or dy) and (not cross or not (dx and dy)) and 0 <= x + dx < 10 and 0 <= y + dy < 10:
                yield y * 10 + dy * 10 + x + dx

def art(e):
    """The marks a player must read and the fleet icons, from the ROM, on the game's own colours."""
    p = e.art("art_shared_pal", 128)
    pal = [(lambda c: (c & 31, c >> 5 & 31, c >> 10 & 31))(p[k * 2] | p[k * 2 + 1] << 8) for k in range(64)]
    pal = [tuple(v * 255 // 31 for v in c) for c in pal]
    def save(data, w, h, name, s):
        im = Image.new("RGB", (w, h))
        im.putdata([pal[v] for v in data])
        im.resize((w * s, h * s), Image.NEAREST).save(OUT + name + ".png")
    big = e.art("art_big", T_COUNT * 144)
    for t, name in ((T_WATER, "water"), (T_MISS, "miss"), (T_HIT, "hit"), (T_KNOWN, "known"),
                    (T_HULLHIT + SEG_H_MID, "hullhit"), (T_SUNK + SEG_H_MID, "sunk")):
        save(big[t * 144:(t + 1) * 144], 12, 12, name, 6)
    icons = e.art("art_icon", 5 * 2 * ICON_W * ICON_H)
    n = ICON_W * ICON_H
    for k in range(5):
        save(icons[k * 2 * n:(k * 2 + 1) * n], ICON_W, ICON_H, f"ship{k + 1}", 3)

def pick(e, rng, peek):
    """A player's next shot: the squares around a hit, else a guess, and sometimes a look at the answer."""
    own, st = e.board("ob")[:2]
    free = [i for i in range(100) if st[i] == S_EMPTY and not any(st[n] == S_SUNK for n in around(i))]
    near = [n for i in range(100) if st[i] == S_HIT for n in around(i, True) if n in free]
    if near and rng.random() < 0.8 + peek: return rng.choice(near)
    ships = [i for i in free if own[i] != EMPTY]
    return rng.choice(ships if ships and rng.random() < peek else free)

def play(e, rng, peek, moments):
    """One game from the first aim. moments says which pictures this game may still take."""
    while True:
        e.battle_ready()
        hits = sum(s >= S_HIT for s in e.board("ob")[1])
        taken = sum(s >= S_HIT for s in e.board("pb")[1])
        sank = any(s == S_SUNK for s in e.board("ob")[1])
        if "battle" in moments and hits >= 7 and taken >= 4 and sank:
            moments.discard("battle")
            e.run(6); snap(e, "battle")
        i = pick(e, rng, peek)
        st = e.board("ob")[1]
        if "known" in moments and sank:     # firing at known water, refused
            ring = [c for c in range(100) if st[c] == S_EMPTY and any(st[n] == S_SUNK for n in around(c))]
            if ring:
                moments.discard("known")
                e.goto(ring[0] % 10, ring[0] // 10)
                e.run(2, A); e.run(4); snap(e, "known-water"); e.run(40)
        e.goto(i % 10, i // 10)
        e.run(2, A); e.run(2)
        r = e.board("ob")[1][i]
        if "shell" in moments:
            moments.discard("shell")
            e.run(15); snap(e, "shell")
        e.land()
        if r == S_HIT and "explosion" in moments:
            moments.discard("explosion"); e.run(8); snap(e, "explosion")
        elif r == S_SUNK and "sinking" in moments:
            moments.discard("sinking"); e.run(16); snap(e, "sinking")
        elif r not in (S_HIT, S_SUNK) and "splash" in moments:
            moments.discard("splash"); e.run(7); snap(e, "splash")
        if not e.until(lambda: e.state() == ST_SPLASH or e.text("status_text") == "ENEMY TURN", 200): raise SystemExit("stuck after a shot")
        if e.state() == ST_SPLASH: return True
        if not e.until(lambda: e.state() == ST_SPLASH or e.text("status_text") == "YOUR TURN", 400): raise SystemExit("stuck after the enemy")
        if e.state() == ST_SPLASH: return False

def main():
    os.makedirs(OUT, exist_ok=True); os.makedirs("shots", exist_ok=True)
    e = Emu("navalhunter.gba")
    rng = random.Random(7)
    art(e)
    e.wait_state(ST_TITLE); e.run(30); snap(e, "title")
    e.tap(START)
    e.wait_state(ST_MENU); e.run(4)
    e.tap(SELECT); e.run(4); snap(e, "menu")            # HARD
    # Placing: the carrier down, then the next ship in the way, then clear of it
    e.tap(B)
    e.wait_state(ST_PLACE); e.run(8)
    e.tap(A); e.run(4); snap(e, "place-no-fit")
    for _ in range(3): e.tap(DOWN)
    for _ in range(2): e.tap(RIGHT)
    e.run(4); snap(e, "place")
    e.tap(START)
    moments = {"shell", "splash", "explosion", "sinking", "known", "battle"}
    want = {True: "win", False: "lose"}
    peek = {True: 0.5, False: 0.03}
    # The first game aims to win. Whichever way a game goes, the next aims at the ending still missing.
    target = True
    for _ in range(12):
        won = play(e, rng, peek[target], moments)
        if won in want:
            e.run(40); snap(e, want[won])
            e.tap(A); e.wait_state(ST_END); e.run(6); snap(e, want[won] + "-end")
            del want[won]
        else:
            e.tap(A); e.wait_state(ST_END); e.run(6)
        if not want and not moments: break
        if want: target = next(iter(want))
        e.tap(START)
        e.wait_state(ST_MENU); e.run(4)
        e.tap(A)
    else:
        raise SystemExit(f"never got: {sorted(want)} {sorted(moments)}")
    e.p.stdin.close(); e.p.wait()
    print("ok")

if __name__ == "__main__":
    main()
