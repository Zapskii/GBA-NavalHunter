# Naval Hunter for the GBA

A full-colour Game Boy Advance port of [GB-NavalHunter](https://github.com/Zapskii/GB-NavalHunter):
turn-based naval combat against the computer. Work in progress, private until release.

Playable from the title to the end screen, with both boards always on screen. All the art is drawn by
`tools/gen_art.py` (needs Pillow, only to change the art: its output `src/art.c` and `src/art.h` is committed, with
a preview in `tools/art.png`).

- Title: START. Menu: A random fleet, B place your own, SELECT cycles the level (EASY, NORMAL, HARD, ADMIRAL).
  ADMIRAL also deals its own fleet with some care, instead of at random, before you do anything.
- Placing: d-pad moves, B turns, A places, START randomises the rest. Ships may not touch.
- Battle: d-pad aims, A fires. Your board, with the enemy's last shot marked, is on the right, over the
  list of enemy ships. A ring is a miss, a spiked cross a hit. Cells around a sunk ship show as dotted water: ships
  never touch, so they cannot be fired at.
- Each shot flies as a shell and lands in a splash or an explosion, or a longer one with a shake if it sinks a ship, and
  damaged ships smoke. A or B skips an animation; a skip never fires a shot and a button held down skips nothing. A
  skip changes nothing in the game: the rules resolve before the shell leaves. ADMIRAL samples against the clock, so its
  own shots can differ from one run to the next if the timing does.
- Game over: the win or lose picture and its jingle for 5 seconds (START or A skips both), then both fleets. START plays again.
- Music is on the PSG: a tune on the title, a quiet loop while you place and fight, silent on the menu and end screen. Effects
  are samples on Direct Sound, one at a time: a splash, an explosion, a long boom for a sinking (a miss doesn't cut it) and a
  sonar ping when SELECT changes the level. `tools/gen_sfx.py` synthesises them (standard library only) into `src/sfx_data.c`.

Build with Docker (`devkitpro/devkitarm`): `make`. `make run` opens mGBA, `make test` checks the rules on the
host, `make e2e` drives the real ROM headlessly. Not yet tested on real hardware.
