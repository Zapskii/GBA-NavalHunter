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

/* Play one full solo game with the AI (queue + line following + hunt) against
   a random fleet, firing at `hard ? ai_prob_pick : ai_hunt` when the queue is
   empty. Returns the number of shots. Also enforces the invariant that every
   partially hit ship always has a queued unfired hull cell to chase. */
static int sim_game(Board *b, uint8_t hard) {
    uint8_t s, i, shots = 0;
    clear_board(b);
    for (s = 0; s < NSHIP; s++)
        if (!place_random_one(b, s)) return -1;        /* skip on fluke */
    ai_clear_queue();
    while (b->alive && shots < 200) {
        /* same shot picker as ai_next(): queue first, then the chosen hunt */
        uint8_t ei = 0xFF, q;
        for (q = 0; q < 100; q++)
            if (target[q]) { target[q] = 0; ei = q; break; }
        if (ei == 0xFF) ei = hard ? ai_prob_pick(b) : ai_hunt(b);
        uint8_t sunk = 0, r;
        r = fire(b, (uint8_t)(ei % 10), (uint8_t)(ei / 10), &sunk);
        shots++;
        CHECK(r != 3, "sim: AI fired at a spent cell");
        if (r == 1) ai_hit(b, b->own[(ei / 10) * 10 + ei % 10], (uint8_t)(ei % 10), (uint8_t)(ei / 10));
        if (r == 2) ai_clear_queue();
        /* invariant: a live partially-hit ship has a queued continuation */
        for (s = 0; s < NSHIP; s++) {
            uint8_t queued = 0;
            if (b->hits[s] == 0 || b->hits[s] >= ship_len[s]) continue;
            for (i = 0; i < 100; i++)
                if (b->own[i] == s && b->st[i] == S_EMPTY && target[i]) queued = 1;
            CHECK(queued, "sim: partially hit ship has no queued continuation");
        }
    }
    return shots;
}

int main(void) {
    Board b;
    uint8_t i, t;
    sim_seed(8);

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
    ai_hit(&b, 2, 5, 4);
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
    ai_hit(&b, 0, 2, 1);
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
    ai_hit(&b, 0, 2, 1);
    CHECK(target[3 * 10 + 2] == 1, "walk passed 3 hits, queued the far end");
    CHECK(target[2 * 10 + 0] == 0, "already-hit cell not re-queued");

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

    /* strength: full solo games, HARD (prob) vs NORMAL (random) hunt, same
       queue/target logic; HARD must win in clearly fewer shots */
    {
        int game, hard_total = 0, rand_total = 0;
        for (game = 0; game < 100; game++) {
            hard_total += sim_game(&b, 1);
            rand_total += sim_game(&b, 0);
        }
        printf("sim: hard %d avg, random %d avg (100 games each)\n",
               hard_total / 100, rand_total / 100);
        CHECK(hard_total < rand_total - 500, "prob hunt clearly beats random hunt");
        (void)hard_total; (void)rand_total;
    }

    printf("test_place: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
