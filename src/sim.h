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

/* A whole random fleet. A very unlucky one starts over rather than spin. */
static void init_board(Board *b) {
    uint8_t s;
    for (;;) {
        clear_board(b);
        for (s = 0; s < NSHIP; s++)
            if (!place_random_one(b, s)) break;
        if (s == NSHIP) return;
    }
}

/* ---------- ADMIRAL's fleet: placed to be harder for a person to find ------
   Deal CLEVER_DEALS random fleets and keep the one with the lowest score, so nothing but the rules
   and chance shapes it and no layout is stored. Few deals on purpose: that is the random element,
   and it is what keeps a hunter who knows this code from learning much. Before play, so no peeking.
   The weights are a judgement, not a measurement of people. The score is, over the hull cells:
   - centre heat, min(x, 9-x) + min(y, 9-y), 0 to 8: an assumption that people shoot the middle first.
   - CLEVER_MOAT per cell of hull and moat on the board: sinking a ship marks its moat as water, so
     a tighter fleet gives every hunter fewer free cells. This holds whatever the hunter assumes.
   No checkerboard term: only the subs could use one, and it measured as easier for an informed hunter.
   More deals or a heavier heat cost plain hunters more, and gave an informed one a bigger lead. */
#define CLEVER_DEALS 4
#define CLEVER_MOAT  2

static uint16_t clever_score(const Board *b) {
    uint16_t row[10], v = 0;
    uint8_t x, y, r;
    for (r = 0; r < 10; r++) row[r] = 0;
    for (y = 0; y < 10; y++)
        for (x = 0; x < 10; x++) {
            if (b->own[y * 10 + x] == CELL_EMPTY) continue;
            v += (uint16_t)((x < 5 ? x : 9 - x) + (y < 5 ? y : 9 - y));
            for (r = y ? y - 1 : 0; r <= y + 1 && r < 10; r++)
                row[r] |= (uint16_t)((7u << x >> 1) & 0x3FF);
        }
    for (r = 0; r < 10; r++)
        for (; row[r]; row[r] &= (uint16_t)(row[r] - 1)) v += CLEVER_MOAT;
    return v;
}

/* `between` runs after each deal, if not NULL. On the ROM a deal takes up to 54% of a frame and all
   four up to 124%, so main.c gives each deal its own frame rather than drop one. */
static void place_fleet_clever(Board *b, void (*between)(void)) {
    Board t;
    uint16_t best = 0xFFFF;
    uint8_t k;
    for (k = 0; k < CLEVER_DEALS; k++) {
        uint16_t v;
        init_board(&t);
        v = clever_score(&t);
        if (v < best) { best = v; *b = t; }
        if (between) between();
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
   # ponytail: per-ship counts ignore multi-ship mutual exclusion. That is the
   # strength HARD is meant to have; ADMIRAL below samples whole fleets instead. */
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

static uint8_t ai_admiral(const Board *b, uint16_t tries);

/* Next shot: a queued follow-up if any, else the hunt the player picked:
   0 = EASY (random), 1 = NORMAL (moat-aware random), 2 = HARD (probability
   map). The queued cell is consumed so it is not fired twice. 3 = ADMIRAL,
   which ignores the queue; here it gets a fixed count of tries, so the same
   seed gives the same shot. The ROM steps it against the clock instead. */
#define MC_TRIES 128
static uint8_t ai_next(const Board *b, uint8_t level) {
    uint8_t i;
    if (level == 3) return ai_admiral(b, MC_TRIES);
    for (i = 0; i < 100; i++)
        if (target[i]) { target[i] = 0; return i; }
    return level == 0 ? ai_random(b)
         : level == 1 ? ai_hunt(b)
         :              ai_prob_pick(b);
}

/* ---------- ADMIRAL: sample whole fleets consistent with what is seen, fire where most hit ------
   mc_begin once a turn, mc_step as often as time allows, then mc_pick. Reads st[] and which ships
   are sunk, nothing else. Boards are one bitmask per row, so a sample costs little on an ARM7.
   A sampled fleet obeys what a player knows: a live ship avoids fired water, sunk ships and their
   moat; no hull touches another; every S_HIT is under a live ship; and no live ship is all hits,
   or it would have sunk. Ships go through the hits first, or nearly every sample is rejected. */
#define MC_MIN_SAMPLES 50       /* fewer valid fleets than this: use the honest queue and hunt */
#define MC_SHIP_TRIES  24       /* placements to try per ship before giving the sample up */

typedef struct { uint8_t x, y, horiz, s; } McShip;

static uint16_t mc_score[100];
static uint16_t mc_samples, mc_tries;
static uint16_t mc_bad[10];     /* per row, the cells no live ship can use */
static uint16_t mc_hits[10];    /* per row, the S_HIT cells */
static McShip mc_fleet[NSHIP];  /* the latest try, as far as it got */
static uint8_t mc_placed;

static void mc_begin(const Board *b) {
    int8_t x, y, dx, dy;
    uint8_t i;
    for (i = 0; i < 100; i++) mc_score[i] = 0;
    for (i = 0; i < 10; i++) mc_bad[i] = mc_hits[i] = 0;
    mc_samples = mc_tries = 0;
    for (y = 0; y < 10; y++)
        for (x = 0; x < 10; x++) {
            uint8_t st = b->st[y * 10 + x];
            if (st == S_HIT) mc_hits[y] |= (uint16_t)(1u << x);
            if (st == S_MISS) mc_bad[y] |= (uint16_t)(1u << x);
            if (st == S_SUNK)
                for (dy = -1; dy <= 1; dy++)
                    for (dx = -1; dx <= 1; dx++)
                        if (y + dy >= 0 && y + dy <= 9 && x + dx >= 0 && x + dx <= 9)
                            mc_bad[y + dy] |= (uint16_t)(1u << (x + dx));
        }
}

/* Ship s at (x,y), if the rules let it go there: the caller keeps it on the board. It claims its
   hull and moat in `used`, and takes the hits it covers out of `left`. */
static uint8_t mc_put(uint16_t *used, uint16_t *left, uint8_t x, uint8_t y, uint8_t horiz, uint8_t s) {
    uint8_t len = ship_len[s], rows = horiz ? 1 : len, r, top, end, unfired = 0;
    uint16_t hull = (uint16_t)(horiz ? ((1u << len) - 1) << x : 1u << x);
    uint16_t wide = (uint16_t)((hull | hull << 1 | hull >> 1) & 0x3FF);
    for (r = y; r < y + rows; r++) {
        if (used[r] & hull) return 0;
        if (hull & ~mc_hits[r]) unfired = 1;
    }
    if (!unfired) return 0;
    top = y ? y - 1 : 0;
    end = y + rows < 10 ? y + rows : 9;
    for (r = top; r <= end; r++)                /* a hit in the moat could never be covered */
        if (mc_hits[r] & wide & ~(r >= y && r < y + rows ? hull : 0)) return 0;
    for (r = top; r <= end; r++) used[r] |= wide;
    for (r = y; r < y + rows; r++) left[r] &= (uint16_t)~hull;
    mc_fleet[mc_placed].x = x; mc_fleet[mc_placed].y = y;
    mc_fleet[mc_placed].horiz = horiz; mc_fleet[mc_placed].s = s;
    mc_placed++;
    return 1;
}

/* A number below n from 10 random bits at `shift` of r. No division: an ARM7 has none. */
#define MC_BELOW(r, shift, n) ((uint8_t)((((r) >> (shift) & 0x3FF) * (uint32_t)(n)) >> 10))

/* Try one fleet. Returns 1 if it was valid, and scores its unfired cells. */
static uint8_t mc_step(const Board *b) {
    uint16_t used[10], left[10];
    uint8_t todo = 0, s, r, k, i;               /* todo: live ships not yet placed, as a bit each */
    mc_tries++;
    mc_placed = 0;
    for (r = 0; r < 10; r++) { used[r] = mc_bad[r]; left[r] = mc_hits[r]; }
    for (s = 0; s < NSHIP; s++)
        if (b->hits[s] < ship_len[s]) todo |= (uint8_t)(1u << s);

    /* Each uncovered hit gets a ship through it. A cell has one placement per unit of length on
       each axis, so picking a unit of length at random makes every placement equally likely. */
    for (r = 0; r < 10; r++)
        while (left[r]) {
            uint8_t hx = 0, sum = 0;
            if (!todo) return 0;                /* more hits than ships can cover */
            while (!(left[r] >> hx & 1)) hx++;
            for (s = 0; s < NSHIP; s++) if (todo >> s & 1) sum += ship_len[s];
            for (k = 0; k < MC_SHIP_TRIES; k++) {
                uint32_t rnd = sim_rand();
                uint8_t pick = MC_BELOW(rnd, 0, sum), horiz = (uint8_t)(rnd >> 10 & 1), len;
                int8_t x = (int8_t)hx, y = (int8_t)r;
                for (s = 0; ; s++) {
                    if (!(todo >> s & 1)) continue;
                    if (pick < ship_len[s]) break;
                    pick -= ship_len[s];
                }
                len = ship_len[s];
                if (horiz) x -= (int8_t)pick; else y -= (int8_t)pick;
                if (x < 0 || y < 0 || (horiz ? x + len : y + len) > 10) continue;
                if (mc_put(used, left, (uint8_t)x, (uint8_t)y, horiz, s)) { todo &= (uint8_t)~(1u << s); break; }
            }
            if (k == MC_SHIP_TRIES) return 0;
        }

    /* The rest go anywhere legal, longest first as the fleet is dealt. */
    for (s = 0; s < NSHIP; s++) {
        uint8_t len = ship_len[s];
        if (!(todo >> s & 1)) continue;
        for (k = 0; k < MC_SHIP_TRIES; k++) {
            uint32_t rnd = sim_rand();
            uint8_t horiz = (uint8_t)(rnd >> 20 & 1);
            uint8_t x = MC_BELOW(rnd, 0, horiz ? 11 - len : 10), y = MC_BELOW(rnd, 10, horiz ? 10 : 11 - len);
            if (mc_put(used, left, x, y, horiz, s)) break;
        }
        if (k == MC_SHIP_TRIES) return 0;
    }

    for (i = 0; i < mc_placed; i++) {
        const McShip *f = &mc_fleet[i];
        uint8_t at = (uint8_t)(f->y * 10 + f->x), step = f->horiz ? 1 : 10;
        for (k = 0; k < ship_len[f->s]; k++, at = (uint8_t)(at + step))
            if (b->st[at] == S_EMPTY) mc_score[at]++;
    }
    mc_samples++;
    return 1;
}

/* Rebuilt rather than trusted: a sinking clears the whole queue, and at this level another ship
   can be partly hit by then, which at the other levels it cannot. */
static void ai_requeue(const Board *b) {
    uint8_t i;
    ai_clear_queue();
    for (i = 0; i < 100; i++)
        if (b->st[i] == S_HIT) ai_hit(b, (uint8_t)(i % 10), (uint8_t)(i / 10));
}

/* The most sampled unfired cell, random among ties. Too few samples, and the honest queue and
   NORMAL's hunt take the shot: HARD's map costs too many frames to run here. */
static uint8_t mc_pick(const Board *b) {
    if (mc_samples >= MC_MIN_SAMPLES) {
        uint8_t start = (uint8_t)(sim_rand() % 100), i, best = 0;
        uint16_t bestv = 0;
        for (i = 0; i < 100; i++) {
            uint8_t idx = (uint8_t)((start + i) % 100);
            if (b->st[idx] == S_EMPTY && mc_score[idx] > bestv) { bestv = mc_score[idx]; best = idx; }
        }
        if (bestv) { target[best] = 0; return best; }
    }
    ai_requeue(b);
    return ai_next(b, 1);
}

static uint8_t ai_admiral(const Board *b, uint16_t tries) {
    mc_begin(b);
    while (tries--) mc_step(b);
    return mc_pick(b);
}

#endif /* PLACE_H */
