/* tests/test_place.c -- host test for the fleet-placement rules.
 *
 * Compiles and runs with plain gcc, NO Game Boy toolchain and no emulator:
 *
 *   gcc -O1 -I.. -o /tmp/test_place tests/test_place.c && /tmp/test_place
 *
 * This includes the REAL place.h that main.c also includes, so these tests can
 * actually fail and the two copies can never drift. Verify the RULES here;
 * verify only the RENDERING in the emulator.
 */
#include <stdio.h>
#include <stdlib.h>
#include "sim.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* Play one full solo game with the AI at `level` against a random fleet. Returns the
   number of shots. Enforces the honest queue's invariant: while any ship is partly hit,
   the queue is not empty and holds only unfired cells. The test may read own[] and
   hits[] to check that; the AI may not. */
static int sim_game(Board *b, uint8_t level) {
    uint8_t s, i, shots = 0;
    clear_board(b);
    for (s = 0; s < NSHIP; s++)
        if (!place_random_one(b, s)) return -1;        /* skip on fluke */
    ai_clear_queue();
    while (b->alive && shots < 200) {
        uint8_t ei = ai_next(b, level), partly = 0, queued = 0, sunk = 0, r;
        if (level > 0)                                 /* EASY's random hunt may fire there */
            CHECK(!is_known_water(b, (uint8_t)(ei % 10), (uint8_t)(ei / 10)), "sim: AI fired at known water");
        r = fire(b, (uint8_t)(ei % 10), (uint8_t)(ei / 10), &sunk);
        shots++;
        CHECK(r != 3, "sim: AI fired at a spent cell");
        if (r == 1) ai_hit(b, (uint8_t)(ei % 10), (uint8_t)(ei / 10));
        if (r == 2) ai_clear_queue();
        for (s = 0; s < NSHIP; s++)
            if (b->hits[s] > 0 && b->hits[s] < ship_len[s]) partly = 1;
        for (i = 0; i < 100; i++)
            if (target[i]) {
                queued = 1;
                CHECK(b->st[i] == S_EMPTY, "sim: queue holds a fired cell");
            }
        CHECK(!partly || queued, "sim: partly hit ship but the queue is empty");
    }
    CHECK(b->alive == 0, "sim: the AI finished the game");
    return shots;
}

/* Every call the AI makes, from a seed, on the state a player can see: first the hunt with
   an empty queue, then the queue rebuilt from the hits and drained. `out` is 101 bytes. */
static void ai_trace(const Board *b, uint8_t level, uint32_t seed, uint8_t *out) {
    uint8_t i, n = 0;
    sim_seed(seed);
    ai_clear_queue();
    out[n++] = ai_next(b, level);
    ai_clear_queue();
    for (i = 0; i < 100; i++)
        if (b->st[i] == S_HIT) ai_hit(b, (uint8_t)(i % 10), (uint8_t)(i / 10));
    while (n < 101) {
        uint8_t q, any = 0;
        for (q = 0; q < 100; q++) if (target[q]) any = 1;
        if (!any) break;
        out[n++] = ai_next(b, level);
    }
    while (n < 101) out[n++] = 0xFF;
}

/* The fairness test: a mid-game board against a twin with the same st[] and the same sunk
   ships, but every own[] cell and every partial hits[] count changed. The AI must not be
   able to tell them apart. Returns how many positions had a ship partly hit, so the caller
   can see the sweep really reached them. */
static int fairness(uint8_t level, int games) {
    int g, partly = 0;
    for (g = 0; g < games; g++) {
        Board a, t;
        uint8_t s, i, seen = 0, ta[101], tt[101], stop;
        sim_seed((uint32_t)(g + 1) * 0x9E3779B1u);
        clear_board(&a);
        for (s = 0; s < NSHIP; s++) if (!place_random_one(&a, s)) break;
        if (s < NSHIP) continue;
        stop = (uint8_t)(sim_rand() % 90);
        ai_clear_queue();
        for (i = 0; i < stop && a.alive > 1; i++) {
            uint8_t e = ai_next(&a, level), sunk = 0, r;
            r = fire(&a, (uint8_t)(e % 10), (uint8_t)(e / 10), &sunk);
            if (r == 1) ai_hit(&a, (uint8_t)(e % 10), (uint8_t)(e / 10));
            if (r == 2) ai_clear_queue();
        }
        t = a;
        /* random, not a swap of hull and water: a swap keeps which neighbours match, and a
           line walk that reads own[] then behaves the same on both boards */
        for (i = 0; i < 100; i++) {
            t.own[i] = (uint8_t)(sim_rand() % (NSHIP + 1));
            if (t.own[i] == NSHIP) t.own[i] = CELL_EMPTY;
        }
        for (s = 0; s < NSHIP; s++) {
            if (a.hits[s] >= ship_len[s]) continue;
            if (a.hits[s]) seen = 1;
            t.hits[s] = (uint8_t)(a.hits[s] ? a.hits[s] - 1 : ship_len[s] - 1);
        }
        partly += seen;
        ai_trace(&a, level, (uint32_t)g + 77, ta);
        ai_trace(&t, level, (uint32_t)g + 77, tt);
        for (i = 0; i < 101; i++)
            if (ta[i] != tt[i]) {
                printf("FAIL: fairness: level %d game %d call %d: %d vs %d\n", level, g, i, ta[i], tt[i]);
                fails++;
                return partly;
            }
    }
    return partly;
}

int main(int argc, char **argv) {
    Board b;
    uint8_t i, t;
    uint32_t seed = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 8;    /* a seed to sweep the checks with */
    sim_seed(seed);

    /* ---- place_ship geometry ---- */
    clear_board(&b);
    place_ship(&b, 2, 3, 5, 1, 0);            /* horizontal, len 5, at (2,3) */
    CHECK(b.own[3*10+2] == 0, "horiz head");
    CHECK(b.own[3*10+6] == 0, "horiz tail");
    CHECK(b.own[3*10+7] == CELL_EMPTY, "horiz past tail empty");
    CHECK(b.own[2*10+2] == CELL_EMPTY, "row above untouched");
    CHECK(b.own[4*10+2] == CELL_EMPTY, "row below untouched");

    clear_board(&b);
    place_ship(&b, 4, 1, 4, 0, 3);            /* vertical, len 4, id 3 */
    CHECK(b.own[1*10+4] == 3, "vert head");
    CHECK(b.own[4*10+4] == 3, "vert tail");
    CHECK(b.own[5*10+4] == CELL_EMPTY, "vert past tail empty");
    CHECK(b.own[1*10+5] == CELL_EMPTY, "col right untouched");

    /* ---- no-touch rule ---- */
    clear_board(&b);
    place_ship(&b, 0, 0, 5, 1, 0);            /* horizontal along row 0 */
    CHECK(can_place(&b, 0, 1, 3, 1) == 0, "row 1 touches row 0 ship");
    CHECK(can_place(&b, 0, 2, 3, 1) == 1, "row 2 is one clear row away");

    clear_board(&b);
    place_ship(&b, 0, 0, 5, 0, 0);            /* vertical along col 0 */
    CHECK(can_place(&b, 1, 0, 5, 0) == 0, "col 1 touches col 0 ship");
    CHECK(can_place(&b, 2, 0, 5, 0) == 1, "col 2 is clear");

    /* ---- board edges ---- */
    clear_board(&b);
    CHECK(can_place(&b, 0, 0, 5, 1) == 1, "len 5 fits from col 0");
    CHECK(can_place(&b, 5, 0, 5, 1) == 1, "len 5 fits ending exactly at col 9");
    CHECK(can_place(&b, 6, 0, 5, 1) == 0, "len 5 from col 6 runs off");
    CHECK(can_place(&b, 0, 6, 5, 0) == 0, "len 5 from row 6 runs off");
    CHECK(can_place(&b, 0, 9, 1, 1) == 1, "len 1 fits in the last row");

    /* ---- a lone corner ship blocks its whole 3x3 ---- */
    clear_board(&b);
    place_ship(&b, 0, 0, 1, 1, 5);
    CHECK(can_place(&b, 1, 0, 1, 1) == 0, "right of corner ship blocked");
    CHECK(can_place(&b, 0, 1, 1, 1) == 0, "below corner ship blocked");
    CHECK(can_place(&b, 1, 1, 1, 1) == 0, "diagonal of corner ship blocked");
    CHECK(can_place(&b, 2, 2, 1, 1) == 1, "cell (2,2) is 2 away, legal");

    /* ---- clear_board resets everything ---- */
    clear_board(&b);
    b.st[0] = 2; b.hits[3] = 4; b.alive = 1;
    clear_board(&b);
    CHECK(b.st[0] == 0 && b.hits[3] == 0 && b.alive == NSHIP,
          "clear_board resets st/hits/alive");

    /* ---- random placement: a full fleet always fits, uses all 7 ids, never touches ---- */
    {
        int trial, ok = 1;
        for (trial = 0; trial < 200 && ok; trial++) {
            uint8_t s, seen[NSHIP];
            int own = 0;
            clear_board(&b);
            for (s = 0; s < NSHIP; s++)
                if (!place_random_one(&b, s)) { ok = 0; printf("FAIL: place_random_one gave up (trial %d ship %d)\n", trial, s); fails++; break; }
            if (!ok) break;
            /* every id present, with the right cell count */
            for (s = 0; s < NSHIP; s++) seen[s] = 0;
            { int i; for (i = 0; i < 100; i++) if (b.own[i] != CELL_EMPTY) { seen[b.own[i]]++; own++; } }
            for (s = 0; s < NSHIP; s++)
                if (seen[s] != ship_len[s]) { ok = 0; printf("FAIL: ship %d has %d cells, expected %d (trial %d)\n", s, seen[s], ship_len[s], trial); fails++; break; }
            if (own != 18) { ok = 0; printf("FAIL: %d occupied cells, expected 18\n", own); fails++; }
        }
    }

    /* ---- ship_is_placed: the fleet panel's placement progress ---- */
    clear_board(&b);
    CHECK(ship_is_placed(&b, 0) == 0, "no ship placed on a clear board");
    CHECK(ship_is_placed(&b, 6) == 0, "last ship not placed either");
    place_ship(&b, 0, 0, 5, 1, 0);
    CHECK(ship_is_placed(&b, 0) == 1, "ship 0 reported placed");
    CHECK(ship_is_placed(&b, 1) == 0, "ship 1 still not placed");
    place_ship(&b, 0, 2, 4, 1, 1);
    CHECK(ship_is_placed(&b, 1) == 1, "ship 1 reported placed after commit");

    /* ---- AI: a lone hit queues the four orthogonal neighbours ---- */
    clear_board(&b);
    place_ship(&b, 4, 4, 3, 1, 2);            /* cruiser horizontal, row 4, cols 4-6 */
    b.st[4 * 10 + 5] = S_HIT; b.hits[2] = 1;
    ai_clear_queue();
    ai_hit(&b, 5, 4);
    CHECK(target[4 * 10 + 4] == 1, "hit queues left neighbour");
    CHECK(target[4 * 10 + 6] == 1, "hit queues right neighbour");
    CHECK(target[3 * 10 + 5] == 1, "hit queues above");
    CHECK(target[5 * 10 + 5] == 1, "hit queues below");
    CHECK(target[3 * 10 + 4] == 0, "diagonal not queued");
    {
        uint8_t n = ai_next(&b, 1);
        CHECK(target[n] == 0, "ai_next consumed a queued cell");
        CHECK(n == 4 * 10 + 4 || n == 4 * 10 + 6 || n == 3 * 10 + 5 || n == 5 * 10 + 5,
              "ai_next returned a queued neighbour");
        n = ai_next(&b, 1);
        CHECK(target[n] == 0, "ai_next returned the next queued cell");
    }

    /* ---- AI: two adjacent hits reveal the line, cross neighbours dropped ---- */
    clear_board(&b);
    place_ship(&b, 2, 0, 5, 0, 0);            /* carrier vertical, col 2, rows 0-4 */
    b.st[2] = S_HIT; b.st[2 * 10 + 1] = S_HIT; b.hits[0] = 2;
    ai_clear_queue();
    ai_hit(&b, 2, 1);
    CHECK(target[2 * 10 + 2] == 1, "line extended down to the next hull cell");
    CHECK(target[1 * 10 + 1] == 0, "left diagonal not queued");
    CHECK(target[1 * 10 + 2] == 0, "left cross not queued");
    CHECK(target[3 * 10 + 2] == 0, "right cross not queued");

    /* ---- AI: the line walk steps over several hits to the far end ---- */
    clear_board(&b);
    place_ship(&b, 2, 0, 5, 0, 0);
    b.st[2] = S_HIT; b.st[2 * 10 + 1] = S_HIT; b.st[2 * 10 + 2] = S_HIT;
    b.hits[0] = 3;
    ai_clear_queue();
    ai_hit(&b, 2, 1);
    CHECK(target[3 * 10 + 2] == 1, "walk passed 3 hits, queued the far end");
    CHECK(target[2 * 10 + 0] == 0, "already-hit cell not re-queued");

    /* ---- AI: it cannot see the hull end, so it fires past it. own[] says (6,4) is water. ---- */
    clear_board(&b);
    place_ship(&b, 3, 4, 3, 1, 2);            /* cruiser, row 4, cols 3-5 */
    b.st[4 * 10 + 4] = S_HIT; b.st[4 * 10 + 5] = S_HIT; b.hits[2] = 2;
    ai_clear_queue();
    ai_hit(&b, 5, 4);
    CHECK(target[4 * 10 + 3] == 1, "line queued on the side with hull");
    CHECK(target[4 * 10 + 6] == 1, "line queued past the hidden end of the hull");

    /* ---- AI: a miss ends the walk on that side ---- */
    b.st[4 * 10 + 6] = S_MISS;
    ai_clear_queue();
    ai_hit(&b, 4, 4);
    CHECK(target[4 * 10 + 3] == 1 && target[4 * 10 + 6] == 0 && target[4 * 10 + 7] == 0, "walk stops at a miss");

    /* ---- AI: the second hit clears the FIRST hit's cross neighbours too ---- */
    clear_board(&b);
    place_ship(&b, 5, 3, 3, 0, 2);            /* cruiser vertical, col 5, rows 3-5 */
    b.st[5 * 10 + 5] = S_HIT;
    ai_clear_queue();
    ai_hit(&b, 5, 5);
    b.st[4 * 10 + 5] = S_HIT;
    ai_hit(&b, 5, 4);
    CHECK(target[3 * 10 + 5] == 1 && target[6 * 10 + 5] == 1, "both line ends queued");
    CHECK(target[5 * 10 + 4] == 0 && target[5 * 10 + 6] == 0, "first hit's cross neighbours cleared");

    /* ---- AI: the queue skips known water ---- */
    clear_board(&b);
    place_ship(&b, 0, 0, 1, 1, 5);
    fire(&b, 0, 0, &t);
    b.st[2 * 10 + 1] = S_HIT;
    ai_clear_queue();
    ai_hit(&b, 1, 2);
    CHECK(target[1 * 10 + 1] == 0, "known water next to a hit not queued");
    CHECK(target[3 * 10 + 1] == 1, "the other neighbours still are");

    /* ---- known water: derived from st[], around sunk cells only ---- */
    clear_board(&b);
    place_ship(&b, 5, 5, 1, 1, 5);
    place_ship(&b, 0, 0, 2, 1, 3);
    {
        uint8_t x, y, sunk = 0;
        CHECK(!is_known_water(&b, 4, 4), "nothing known before a sinking");
        CHECK(fire(&b, 0, 0, &sunk) == 1, "a hit, not sunk");
        CHECK(!is_known_water(&b, 0, 1), "a hit marks no water");
        b.st[4 * 10 + 4] = S_MISS;
        fire(&b, 5, 5, &sunk);
        for (y = 3; y <= 7; y++)
            for (x = 3; x <= 7; x++) {
                uint8_t ring = x >= 4 && x <= 6 && y >= 4 && y <= 6 && !(x == 5 && y == 5) && !(x == 4 && y == 4);
                CHECK(is_known_water(&b, x, y) == ring, "known water is exactly the unfired 3x3 around a sunk cell");
            }
        /* no new st[] value: fire() still takes the cell, as a counted miss (EASY does this) */
        CHECK(b.st[4 * 10 + 5] == S_EMPTY, "known water stays S_EMPTY in st[]");
        CHECK(fire(&b, 5, 4, &sunk) == 0 && b.st[4 * 10 + 5] == S_MISS, "fire treats known water as a plain miss");
    }
    clear_board(&b);
    place_ship(&b, 0, 0, 1, 1, 5);
    fire(&b, 0, 0, &t);
    CHECK(is_known_water(&b, 1, 1) && !is_known_water(&b, 2, 0) && !is_known_water(&b, 0, 0), "a corner: the moat stops at the edge");

    /* ---- every level plays out with the queue invariant held (sim_game's CHECKs) ---- */
    for (t = 0; t < 3; t++) {
        int game;
        for (game = 0; game < 100; game++) sim_game(&b, t);
    }

    /* ---- the fairness test, at every level (Phase 4's level gets added here) ---- */
    for (t = 0; t < 3; t++) {
        int partly = fairness(t, 1000);
        printf("fairness: level %d, %d positions with a ship partly hit\n", t, partly);
        CHECK(partly > 100, "fairness sweep reached partly hit ships");
    }

    /* ---- AI: hunt skips dead cells next to a sunk ship ---- */
    clear_board(&b);
    place_ship(&b, 0, 0, 4, 0, 1);            /* vertical col 0, rows 0-3 */
    for (i = 0; i < 4; i++) b.st[i * 10] = S_SUNK;
    b.hits[1] = 4; b.alive = 6;
    for (t = 0; t < 200; t++) {
        uint8_t idx = ai_hunt(&b);
        CHECK(b.st[idx] == S_EMPTY, "hunt fired at an unfired cell");
        /* the sunk ship sits on col 0 rows 0-3, so its 3x3 moat covers
           col 0..1 x rows 0..4 -- everything else is a legal shot */
        CHECK(!(idx % 10 <= 1 && idx / 10 <= 4), "hunt avoided a cell touching a sunk ship");
        if (fails) break;
    }

    /* ---- AI: probability hunt ---- */
    /* MISS wall down column 0 -> the play space is cols 1-9, and the most
       placements of the long ships pass through column 5. Unique argmax. */
    clear_board(&b);
    for (i = 0; i < 10; i++) b.st[i * 10] = S_MISS;
    for (t = 0; t < 50; t++)
        CHECK(ai_prob_pick(&b) % 10 == 5, "prob hunt prefers the densest column");

    /* never fires a spent cell or a moated one */
    clear_board(&b);
    place_ship(&b, 0, 0, 4, 0, 1);
    for (i = 0; i < 4; i++) b.st[i * 10] = S_SUNK;
    b.hits[1] = 4; b.alive = 6;
    for (t = 0; t < 200; t++) {
        uint8_t idx = ai_prob_pick(&b);
        CHECK(b.st[idx] == S_EMPTY, "prob hunt fired at an unfired cell");
        CHECK(!(idx % 10 <= 1 && idx / 10 <= 4), "prob hunt avoided a moated cell");
        if (fails) break;
    }

    /* strength: full solo games, HARD (prob) vs NORMAL (random) hunt, same queue logic. Over
       2000 games and 60 seeds the shots saved per game measured 4.77 to 6.31, and -0.68 to
       0.62 with ai_prob_pick neutered. The bar of 2.75 sits in the middle of that gap. */
    {
        int game, hard_total = 0, rand_total = 0;
        sim_seed(seed);                       /* fairness() above seeds for itself */
        for (game = 0; game < 2000; game++) {
            hard_total += sim_game(&b, 2);
            rand_total += sim_game(&b, 1);
        }
        printf("sim: hard %.2f avg, random %.2f avg (2000 games each)\n",
               hard_total / 2000.0, rand_total / 2000.0);
        CHECK(rand_total - hard_total > 5500, "prob hunt clearly beats random hunt");
    }

    printf("test_place: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
