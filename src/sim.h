/* place.h -- fleet-placement RULES, shared by main.c and tests/test_place.c.
 *
 * WHY THIS IS A SEPARATE HEADER: these two functions are pure board logic -- no
 * GB hardware, no GBDK calls. Keeping them out of main.c means the host test can
 * include the REAL implementation instead of a copy, so the test can actually
 * fail, and the two copies can never drift.
 *
 * Verified GB-header-free: this header includes only <stdint.h>, and
 * tests/test_place.c compiles and runs with plain gcc.
 */
#ifndef PLACE_H
#define PLACE_H

#include <stdint.h>

/* xorshift32, so host and ROM share one generator. A zero state would stay zero
   for ever, so a zero seed becomes 1. */
static uint32_t sim_state = 1;

static void sim_seed(uint32_t seed) { sim_state = seed ? seed : 1; }

static uint32_t sim_rand(void) {
    sim_state ^= sim_state << 13;
    sim_state ^= sim_state >> 17;
    sim_state ^= sim_state << 5;
    return sim_state;
}

#define CELL_EMPTY 255          /* also doubles as "no ship" / NOSHIP */
#define NSHIP      7

/* cell firing states (st[]) */
#define S_EMPTY 0
#define S_MISS  1
#define S_HIT   2
#define S_SUNK  3

/* Ship lengths in placement order: carrier, battleship, cruiser, 2 destroyers,
   2 subs. Declared here (not in main.c) so the rules can use it. */
static const uint8_t ship_len[NSHIP] = {5, 4, 3, 2, 2, 1, 1};

/* Board is a plain 10x10 grid. own[] holds the ship id of each cell, or
   CELL_EMPTY. st[] holds firing state (S_EMPTY/S_MISS/S_HIT/S_SUNK). */
typedef struct {
    uint8_t own[100];
    uint8_t st[100];
    uint8_t hits[NSHIP];
    uint8_t alive;
} Board;

/* 1 if a ship of `len` fits at (x,y) horizontally (horiz!=0) or vertically,
   WITHOUT touching any existing ship -- the rules need a 1-cell moat,
   implemented by scanning the 3x3 neighbourhood of every hull cell.
   Returns 0 if the hull would run off the board. */
static uint8_t can_place(const Board *b, uint8_t x, uint8_t y, uint8_t len, uint8_t horiz) {
    uint8_t k, dx, dy;
    for (k = 0; k < len; k++) {
        uint8_t nx = horiz ? x + k : x;
        uint8_t ny = horiz ? y : y + k;
        if (nx > 9 || ny > 9) return 0;
        for (dy = 0; dy < 3; dy++)
            for (dx = 0; dx < 3; dx++) {
                int8_t sx = (int8_t)nx + (int8_t)dx - 1;
                int8_t sy = (int8_t)ny + (int8_t)dy - 1;
                if (sx < 0 || sy < 0 || sx > 9 || sy > 9) continue;
                if (b->own[sy * 10 + sx] != CELL_EMPTY) return 0;
            }
    }
    return 1;
}

/* Write one ship into the board. The caller MUST have verified can_place()
   first -- this function does no checking and will happily overwrite. */
static void place_ship(Board *b, uint8_t x, uint8_t y, uint8_t len,
                       uint8_t horiz, uint8_t id) {
    uint8_t k;
    for (k = 0; k < len; k++)
        b->own[(horiz ? y : y + k) * 10 + (horiz ? x + k : x)] = id;
}

/* 1 if ship `s` has any cell on the board (i.e. it has been placed).
   Used by the fleet panel during manual placement to show progress: only ships
   already committed should appear, otherwise the panel claims a full fleet. */
static uint8_t ship_is_placed(const Board *b, uint8_t s) {
    uint8_t i;
    for (i = 0; i < 100; i++)
        if (b->own[i] == s) return 1;
    return 0;
}

/* Empty `own` and reset `st`/`hits`/`alive` so the board is ready to place. */
static void clear_board(Board *b) {
    uint8_t i;
    for (i = 0; i < 100; i++) { b->own[i] = CELL_EMPTY; b->st[i] = 0; }
    for (i = 0; i < NSHIP; i++) b->hits[i] = 0;
    b->alive = NSHIP;
}

/* Randomly place ONE ship (index s). Returns 1 on success, 0 if it gave up.
   Used by init_board and by the manual-placement "skip the rest" action. */
static uint8_t place_random_one(Board *b, uint8_t s) {
    uint8_t tries = 0;
    for (;;) {
        uint8_t horiz = (uint8_t)(sim_rand() & 1);
        uint8_t x = (uint8_t)(sim_rand() % 10);
        uint8_t y = (uint8_t)(sim_rand() % 10);
        if (can_place(b, x, y, ship_len[s], horiz)) {
            place_ship(b, x, y, ship_len[s], horiz, s);
            return 1;
        }
        if (++tries > 200) return 0;
    }
}

/* Fire at (x,y). Returns:
 *   0 = miss
 *   1 = hit
 *   2 = hit + ship sunk
 *   3 = ILLEGAL: that square was already fired at (caller must not consume a turn)
 */
static uint8_t fire(Board *b, uint8_t x, uint8_t y, uint8_t *sunk) {
    uint8_t i = y * 10 + x, o;
    if (b->st[i] != S_EMPTY) return 3;      /* already fired here */
    o = b->own[i];
    if (o == CELL_EMPTY) { b->st[i] = S_MISS; return 0; }

    b->st[i] = S_HIT;
    b->hits[o]++;
    if (b->hits[o] >= ship_len[o]) {
        uint8_t k;
        for (k = 0; k < 100; k++) if (b->own[k] == o) b->st[k] = S_SUNK;
        b->alive--;
        *sunk = o;
        return 2;
    }
    return 1;
}

/* ---------- computer opponent (pure logic, host-testable like the rules) ------
 * HUNT: random cell. A cell next to a KNOWN ship (hit or sunk) can hold no
   ship -- ships never touch -- so those dead shots are skipped while any
   other cell remains. The random start kills the old every-other-slot parity
   rhythm; the moat rule replaces the accuracy it bought.
 * HARD HUNT: a probability map replaces the random hunt -- see ai_prob_pick.
 * TARGET: after a hit, try the four neighbours; once a second hit is adjacent,
   the orientation is known and we extend the line in both directions, the way
   a human finishes off a damaged ship. Like a human it cannot see where the
   hull ends, so it finds out by firing past the end. All of this reads st[]
   only; the one thing it reads from hits[] is whether a ship is sunk.
 * target[] queues the cells to fire before hunting resumes; sinking the ship
   clears it, because every neighbour of a sunk ship is provably water.
 * ai_next() is only a hunt with an EMPTY queue, which means no ship is
   partially hit -- so the moat check (st >= S_HIT, sunk only) is exact: it
   can never mark a live ship's continuation as dead.
 */
static uint8_t target[100];

static void ai_clear_queue(void) {
    uint8_t i;
    for (i = 0; i < 100; i++) target[i] = 0;
}

/* 1 if (x,y) or any of its 8 neighbours has reached firing state `st`. */
static uint8_t ai_near(const Board *b, uint8_t x, uint8_t y, uint8_t st) {
    int8_t dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            int8_t nx = (int8_t)(x + dx), ny = (int8_t)(y + dy);
            if (nx < 0 || nx > 9 || ny < 0 || ny > 9) continue;
            if (b->st[ny * 10 + nx] >= st) return 1;
        }
    return 0;
}

/* 1 if any of the 8 neighbours of (x,y) is a known ship cell. */
static uint8_t ai_moated(const Board *b, uint8_t x, uint8_t y) {
    return ai_near(b, x, y, S_HIT);
}

/* An unfired cell next to a sunk ship: ships never touch, so it can only be water.
   Derived, not a st[] value: fire() and the shot count mean what they always did. */
static uint8_t is_known_water(const Board *b, uint8_t x, uint8_t y) {
    return b->st[y * 10 + x] == S_EMPTY && ai_near(b, x, y, S_SUNK);
}

static void ai_queue(const Board *b, uint8_t x, uint8_t y) {
    if (x > 9 || y > 9) return;
    if (b->st[y * 10 + x] != S_EMPTY || is_known_water(b, x, y)) return;
    target[y * 10 + x] = 1;
}

static void ai_unqueue(uint8_t x, uint8_t y) {
    if (x <= 9 && y <= 9) target[y * 10 + x] = 0;
}

/* Queue the continuation of a line of hits along one axis from the hit at (x,y).
   The walk steps over hits, which may have landed in any order, and queues the
   first unfired cell each way. A miss, a sunk cell or the edge ends it. Cells beside
   a hit, across the line, are water since ships never touch: unqueue them for every
   hit in the line, because the first hit queued its own neighbours. */
static void ai_extend(const Board *b, uint8_t x, uint8_t y, uint8_t horiz) {
    int8_t sgn;
    for (sgn = -1; sgn <= 1; sgn += 2) {
        int8_t px = (int8_t)x, py = (int8_t)y;
        for (;;) {
            int8_t nx = horiz ? (int8_t)(px + sgn) : px;
            int8_t ny = horiz ? py : (int8_t)(py + sgn);
            ai_unqueue(horiz ? (uint8_t)px : (uint8_t)(px - 1), horiz ? (uint8_t)(py - 1) : (uint8_t)py);
            ai_unqueue(horiz ? (uint8_t)px : (uint8_t)(px + 1), horiz ? (uint8_t)(py + 1) : (uint8_t)py);
            if (nx < 0 || nx > 9 || ny < 0 || ny > 9) break;
            if (b->st[ny * 10 + nx] == S_EMPTY) ai_queue(b, (uint8_t)nx, (uint8_t)ny);
            if (b->st[ny * 10 + nx] != S_HIT) break;
            px = nx; py = ny;
        }
    }
}

/* Follow-up after a hit at (x,y). Ships never touch, so any adjacent hit is
   part of the same ship and reveals the orientation. */
static void ai_hit(const Board *b, uint8_t x, uint8_t y) {
    uint8_t horiz = 0xFF;                            /* unknown */
    if ((x > 0 && b->st[y * 10 + x - 1] == S_HIT) ||
        (x < 9 && b->st[y * 10 + x + 1] == S_HIT)) horiz = 1;
    else if ((y > 0 && b->st[(y - 1) * 10 + x] == S_HIT) ||
             (y < 9 && b->st[(y + 1) * 10 + x] == S_HIT)) horiz = 0;
    if (horiz != 0xFF) ai_extend(b, x, y, horiz);
    else {
        ai_queue(b, (uint8_t)(x + 1), y); ai_queue(b, (uint8_t)(x - 1), y);
        ai_queue(b, x, (uint8_t)(y + 1)); ai_queue(b, x, (uint8_t)(y - 1));
    }
}

/* HUNT: random unfired cell, moat-aware. Two passes so the loop always
   terminates: preferred (not moated), then any unfired cell. */
static uint8_t ai_hunt(const Board *b) {
    uint8_t start = (uint8_t)(sim_rand() % 100);
    uint8_t pass, i;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < 100; i++) {
            uint8_t idx = (uint8_t)((start + i) % 100);
            uint8_t x = (uint8_t)(idx % 10), y = (uint8_t)(idx / 10);
            if (b->st[idx] != S_EMPTY) continue;
            if (pass == 0 && ai_moated(b, x, y)) continue;
            return idx;
        }
    return 0;   /* only reachable once every cell has been fired */
}

/* HARD hunt: a probability map. For every still-alive ship, slide it across
   every position and orientation and count the placements that could still
   hold it; the cell covered by the most placements is the best next shot.
   A placement fits only if every hull cell is unfired and the whole hull
   respects the moat of every KNOWN ship cell (ships never touch).
   # ponytail: per-ship counts ignore multi-ship mutual exclusion; the
   # standard upgrade is iterative re-weighting, only if the sim shows misses
   # in endgame parity with solvers. */
static uint8_t ai_prob_pick(const Board *b) {
    uint16_t score[100];
    uint16_t bestv = 0;
    uint8_t s, i, best = 0;
    for (i = 0; i < 100; i++) score[i] = 0;
    for (s = 0; s < NSHIP; s++) {
        uint8_t len = ship_len[s], horiz;
        if (b->hits[s] >= len) continue;               /* sunk */
        for (horiz = 0; horiz <= 1; horiz++)
            for (i = 0; i < 100; i++) {
                uint8_t x = (uint8_t)(i % 10), y = (uint8_t)(i / 10);
                uint8_t k, ok = 1;
                if (horiz ? (x > 10 - len) : (y > 10 - len)) continue;
                for (k = 0; k < len; k++) {
                    uint8_t hx = horiz ? (uint8_t)(x + k) : x;
                    uint8_t hy = horiz ? y : (uint8_t)(y + k);
                    if (b->st[hy * 10 + hx] != S_EMPTY || ai_moated(b, hx, hy)) {
                        ok = 0;
                        break;
                    }
                }
                if (ok)
                    for (k = 0; k < len; k++)
                        score[horiz ? (uint8_t)(y * 10 + x + k)
                                    : (uint8_t)((y + k) * 10 + x)]++;
            }
    }
    /* max score, random tiebreak via the random scan start */
    {
        uint8_t start = (uint8_t)(sim_rand() % 100);
        for (i = 0; i < 100; i++) {
            uint8_t idx = (uint8_t)((start + i) % 100);
            if (b->st[idx] != S_EMPTY) continue;       /* never re-fire */
            if (score[idx] > bestv) { bestv = score[idx]; best = idx; }
        }
    }
    return bestv ? best : ai_hunt(b);                  /* safety fallback */
}

/* EASY hunt: pure random unfired cell, known water included. Firing there is
   part of being easy; the other levels' hunts skip the moat. */
static uint8_t ai_random(const Board *b) {
    uint8_t start = (uint8_t)(sim_rand() % 100);
    uint8_t i;
    for (i = 0; i < 100; i++) {
        uint8_t idx = (uint8_t)((start + i) % 100);
        if (b->st[idx] == S_EMPTY) return idx;
    }
    return 0;
}

/* Next shot: a queued follow-up if any, else the hunt the player picked:
   0 = EASY (random), 1 = NORMAL (moat-aware random), 2 = HARD (probability
   map). The queued cell is consumed so it is not fired twice. */
static uint8_t ai_next(const Board *b, uint8_t level) {
    uint8_t i;
    for (i = 0; i < 100; i++)
        if (target[i]) { target[i] = 0; return i; }
    return level == 0 ? ai_random(b)
         : level == 1 ? ai_hunt(b)
         :              ai_prob_pick(b);
}

#endif /* PLACE_H */
