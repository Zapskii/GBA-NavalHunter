# Naval Hunter for the GBA

A full-colour Game Boy Advance port of [GB-NavalHunter](https://github.com/Zapskii/GB-NavalHunter):
turn-based naval combat against the computer. Work in progress, private until release.

Playable from the title to the end screen, with both boards always on screen, in placeholder art.

- Title: START. Menu: A random fleet, B place your own, SELECT cycles the level (EASY, NORMAL, HARD).
- Placing: d-pad moves, B turns, A places, START randomises the rest. Ships may not touch.
- Battle: d-pad aims, A fires. Your board, with the enemy's last shot marked, is on the right, over the
  list of enemy ships.
- End: START plays again.

Build with Docker (`devkitpro/devkitarm`): `make`. `make run` opens mGBA, `make test` checks the rules on the
host, `make e2e` drives the real ROM headlessly. Not yet tested on real hardware.
