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
#include <string.h>
#include "sim.h"
#include "gfx.c"        /* tonc-free on purpose, so its rules are tested here against the same code */

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* gfx_segment: all five lengths both ways round, and gfx_seg_of must agree with it cell for cell. */
static void segment_checks(void) {
    Board b;
    int len, h, k;
    for (len = 1; len <= 5; len++)
        for (h = 0; h <= 1; h++) {
            clear_board(&b);
            place_ship(&b, 2, 3, (uint8_t)len, (uint8_t)h, 0);
            for (k = 0; k < len; k++) {
                int i = h ? 3 * 10 + 2 + k : (3 + k) * 10 + 2;
                int want = len == 1 ? SEG_BOAT
                         : h ? (k == 0 ? SEG_H_STERN : k == len - 1 ? SEG_H_BOW : SEG_H_MID)
                             : (k == 0 ? SEG_V_BOW : k == len - 1 ? SEG_V_STERN : SEG_V_MID);
                CHECK(gfx_segment(b.own, i) == want, "segment from neighbours");
                CHECK(gfx_seg_of(k, len, h) == want, "segment of a preview");
            }
        }
    /* other ids beside it are not neighbours, at the board's edges too */
    clear_board(&b);
    place_ship(&b, 0, 0, 2, 1, 0);
    place_ship(&b, 2, 0, 2, 1, 1);
    place_ship(&b, 0, 1, 2, 1, 2);
    CHECK(gfx_segment(b.own, 1) == SEG_H_BOW, "a different id ahead does not extend a hull");
    CHECK(gfx_segment(b.own, 2) == SEG_H_STERN, "nor one behind");
    clear_board(&b);
    place_ship(&b, 9, 0, 1, 1, 0);
    place_ship(&b, 0, 1, 1, 1, 0);      /* the next index, but not the next cell: another row */
    CHECK(gfx_segment(b.own, 9) == SEG_BOAT && gfx_segment(b.own, 10) == SEG_BOAT, "a row's end does not wrap");
}

/* gfx_blit at odd and even x, wide and narrow: every pixel lands, and nothing around it changes. */
static void blit_checks(void) {
    static uint16_t fb[GFX_W * GFX_H / 2];
    static const uint8_t src[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    int x, w, px, py;
    gfx_fb = fb;
    for (x = 4; x <= 5; x++)
        for (w = 1; w <= 4; w++) {
            memset(fb, 0xEE, sizeof fb);
            gfx_blit(x, 2, w, 2, src, 6);
            for (py = 0; py < 6; py++)
                for (px = 0; px < 12; px++) {
                    int got = fb[py * GFX_W / 2 + px / 2] >> (px & 1 ? 8 : 0) & 0xFF, in = px >= x && px < x + w && (py == 2 || py == 3);
                    CHECK(got == (in ? src[(py - 2) * 6 + px - x] : 0xEE), "gfx_blit pixel");
                }
        }
}

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
        /* ADMIRAL can have two ships partly hit, and a sinking clears the queue for both, so
           what must hold is the queue mc_pick falls back on: the rebuilt one. */
        if (level == 3) ai_requeue(b);
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

/* Deal a game the way measure.c does, so every level meets the same fleets. */
static void deal(Board *b, int g) {
    sim_seed((uint32_t)(g + 1) * 0x9E3779B1u);
    init_board(b);
}

/* A whole fleet, judged from own[] and none of the placing code: every ship once, a straight unbroken
   line of its length, touching no other hull, on a board ready to play. */
static int fleet_legal(const Board *b) {
    uint8_t seen[NSHIP] = {0};
    int i, dx, dy, s;
    for (i = 0; i < 100; i++) {
        uint8_t o = b->own[i];
        if (b->st[i] != S_EMPTY) return 0;
        if (o == CELL_EMPTY) continue;
        if (o >= NSHIP) return 0;
        seen[o]++;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                int x = i % 10 + dx, y = i / 10 + dy;
                if (x < 0 || x > 9 || y < 0 || y > 9) continue;
                if (b->own[y * 10 + x] != CELL_EMPTY && b->own[y * 10 + x] != o) return 0;
            }
    }
    for (s = 0; s < NSHIP; s++) {
        int lo = 100, hi = -1;
        if (seen[s] != ship_len[s] || b->hits[s]) return 0;
        for (i = 0; i < 100; i++) if (b->own[i] == s) { if (i < lo) lo = i; hi = i; }
        if (hi - lo != (ship_len[s] - 1) && hi - lo != (ship_len[s] - 1) * 10) return 0;
        if (hi - lo == ship_len[s] - 1 && lo / 10 != hi / 10) return 0;
    }
    return b->alive == NSHIP;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* ADMIRAL's fleet placement. The bounds sit outside what 20,000 measured fleets gave: cells 12.2% to
   24.4%, 45.6% of hull on the edge. */
static void clever_checks(void) {
    enum { N = 2000 };
    static uint32_t hash[N];
    uint32_t occ[100] = {0}, h = 2166136261u, st;
    long clever_total = 0, random_total = 0, edge = 0;
    int g, i, k, distinct = 1, legal = 0;
    Board b, t;

    /* The other levels and the player's random fleet: from HEAD before this change, fleets of
       1000 seeds and the generator's state after each. A changed deal or sim_rand order fails. */
    for (g = 0; g < 1000; g++) {
        deal(&b, g);
        for (i = 0; i < 100; i++) h = (h ^ b.own[i]) * 16777619u;
        h = (h ^ sim_rand()) * 16777619u;
    }
    CHECK(h == 0x128d38de, "init_board deals the fleets it always did, in the same sim_rand order");

    /* The score, on fleets worked out by hand, the weights spelled out: changing one should fail here. */
    clear_board(&b);
    place_ship(&b, 0, 0, 1, 1, 5);
    CHECK(clever_score(&b) == 0 + 4 * 2, "a corner sub: no heat, 4 cells of hull and moat");
    clear_board(&b);
    place_ship(&b, 4, 5, 1, 1, 5);
    CHECK(clever_score(&b) == 8 + 9 * 2, "a middle sub: all the heat, 9 cells");
    clear_board(&b);
    place_ship(&b, 2, 2, 1, 1, 5);
    place_ship(&b, 2, 4, 1, 1, 6);              /* moats share a row of 3 */
    CHECK(clever_score(&b) == 4 + 6 + 15 * 2, "two near subs: a shared moat counts once");
    clear_board(&b);
    place_ship(&b, 0, 9, 5, 1, 0);
    CHECK(clever_score(&b) == 0 + 1 + 2 + 3 + 4 + 12 * 2, "a carrier on the bottom edge");

    /* It keeps the lowest of exactly the deals init_board would give from that seed. */
    for (g = 0; g < 300; g++) {
        uint16_t lowest = 0xFFFF;
        Board keep;
        sim_seed((uint32_t)(g + 1) * 0x9E3779B1u);
        for (k = 0; k < CLEVER_DEALS; k++) {
            init_board(&t);
            if (clever_score(&t) < lowest) { lowest = clever_score(&t); keep = t; }
        }
        st = sim_state;
        sim_seed((uint32_t)(g + 1) * 0x9E3779B1u);
        place_fleet_clever(&b, NULL);
        CHECK(sim_state == st, "it takes exactly CLEVER_DEALS deals of the generator");
        CHECK(!memcmp(b.own, keep.own, 100), "it keeps the lowest score, the first on a tie");
    }

    for (g = 0; g < N; g++) {
        uint32_t f = 2166136261u;
        sim_seed((uint32_t)(g + 1) * 0xDEADBEEFu);
        place_fleet_clever(&b, NULL);
        legal += fleet_legal(&b);
        clever_total += clever_score(&b);
        for (i = 0; i < 100; i++) {
            f = (f ^ b.own[i]) * 16777619u;
            if (b.own[i] != CELL_EMPTY) {
                occ[i]++;
                if (i % 10 == 0 || i % 10 == 9 || i < 10 || i >= 90) edge++;
            }
        }
        hash[g] = f;
        init_board(&t);
        random_total += clever_score(&t);
    }
    CHECK(legal == N, "every clever fleet is legal and complete");
    qsort(hash, N, sizeof hash[0], cmp_u32);
    for (g = 1; g < N; g++) distinct += hash[g] != hash[g - 1];
    printf("clever: %d of %d layouts distinct, %.1f%% of hull on the edge, score %.1f v %.1f at random\n",
           distinct, N, edge * 100.0 / (N * 18), (double)clever_total / N, (double)random_total / N);
    CHECK(distinct >= N - 2, "clever fleets do not repeat");
    /* Not a habit: no cell near certain or near never, and the edge only somewhat favoured. */
    for (i = 0; i < 100; i++)
        CHECK(occ[i] >= N * 8 / 100 && occ[i] <= N * 30 / 100, "no cell is a habit of the clever fleet");
    CHECK(edge < (long)N * 18 * 52 / 100, "the clever fleet does not hug the edge");
    /* And it is doing something. Means over 2000: about 203 at best of 4, 217 at random. */
    CHECK(clever_total + (long)N * 5 < random_total, "clever fleets score lower than random ones");
}

/* Judge the fleet mc_step just made, from st[] and sunk status alone and none of its code: every
   live ship once, on the board, off fired water and off anything by a sunk ship, touching no
   other hull, not all hits, and every hit covered. Adds the fleet's unfired cells to score. */
static int mc_fleet_legal(const Board *b, uint32_t *score) {
    int8_t grid[100];
    uint8_t seen = 0, want = 0;
    int i, k, dx, dy;
    for (i = 0; i < 100; i++) grid[i] = -1;
    for (i = 0; i < NSHIP; i++) if (b->hits[i] < ship_len[i]) want |= (uint8_t)(1u << i);
    for (i = 0; i < mc_placed; i++) {
        const McShip *f = &mc_fleet[i];
        int unfired = 0;
        if (seen >> f->s & 1) return 0;
        seen |= (uint8_t)(1u << f->s);
        for (k = 0; k < ship_len[f->s]; k++) {
            int x = f->x + (f->horiz ? k : 0), y = f->y + (f->horiz ? 0 : k);
            if (x > 9 || y > 9 || grid[y * 10 + x] != -1) return 0;
            if (b->st[y * 10 + x] == S_MISS || b->st[y * 10 + x] == S_SUNK) return 0;
            if (b->st[y * 10 + x] == S_EMPTY) unfired = 1;
            grid[y * 10 + x] = (int8_t)f->s;
        }
        if (!unfired) return 0;
    }
    if (seen != want) return 0;
    for (i = 0; i < 100; i++) {
        if (b->st[i] == S_HIT && grid[i] == -1) return 0;
        if (grid[i] == -1) continue;
        if (b->st[i] == S_EMPTY) score[i]++;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                int x = i % 10 + dx, y = i / 10 + dy;
                if (x < 0 || x > 9 || y < 0 || y > 9) continue;
                if (b->st[y * 10 + x] == S_SUNK) return 0;
                if (grid[y * 10 + x] != -1 && grid[y * 10 + x] != grid[i]) return 0;
            }
    }
    return 1;
}

/* An ADMIRAL shot must be unfired and not known water, unless known water is all that is left. */
static void admiral_shot_ok(const Board *b, uint8_t i, const char *what) {
    uint8_t c, other = 0;
    if (i >= 100 || b->st[i] != S_EMPTY) { printf("FAIL: %s: fired at spent cell %d\n", what, i); fails++; return; }
    for (c = 0; c < 100; c++)
        if (b->st[c] == S_EMPTY && !is_known_water(b, (uint8_t)(c % 10), (uint8_t)(c / 10))) other = 1;
    if (other && is_known_water(b, (uint8_t)(i % 10), (uint8_t)(i / 10))) {
        printf("FAIL: %s: fired at known water %d\n", what, i);
        fails++;
    }
}

static void admiral_checks(void) {
    Board b;
    int g, k;
    uint8_t i, x, y, sunk = 0;

    /* every sampled fleet is legal and covers the hits, through whole games. mc_score holds what
       the valid fleets say and nothing else. */
    {
        long fleets = 0, with_hits = 0, two_damaged = 0;
        for (g = 0; g < 200; g++) {
            int n = 0;
            deal(&b, g);
            ai_clear_queue();
            while (b.alive && n++ < 200) {
                uint32_t score[100] = {0};
                uint8_t any_hit = 0, damaged = 0, s, pick;
                for (s = 0; s < NSHIP; s++) if (b.hits[s] && b.hits[s] < ship_len[s]) damaged++;
                for (i = 0; i < 100; i++) if (b.st[i] == S_HIT) any_hit = 1;
                mc_begin(&b);
                for (k = 0; k < 64; k++)
                    if (mc_step(&b)) {
                        fleets++;
                        with_hits += any_hit;
                        two_damaged += damaged > 1;
                        if (!mc_fleet_legal(&b, score)) { printf("FAIL: sampled fleet is illegal (game %d)\n", g); fails++; return; }
                    }
                for (i = 0; i < 100; i++)
                    if (mc_score[i] != score[i]) { printf("FAIL: mc_score[%d] %d, the valid fleets say %d\n", i, mc_score[i], (int)score[i]); fails++; return; }
                pick = mc_pick(&b);
                admiral_shot_ok(&b, pick, "sampled game");
                {
                    uint8_t r = fire(&b, (uint8_t)(pick % 10), (uint8_t)(pick / 10), &sunk);
                    if (r == 1) ai_hit(&b, (uint8_t)(pick % 10), (uint8_t)(pick / 10));
                    if (r == 2) ai_clear_queue();
                }
            }
            CHECK(b.alive == 0, "ADMIRAL finishes a game");
        }
        printf("admiral: %ld sampled fleets judged legal, %ld with a ship partly hit, %ld with two\n", fleets, with_hits, two_damaged);
        CHECK(with_hits > 10000, "the property sweep reached partly hit ships");
    }

    /* a partly hit ship: every fleet runs on from a lone hit, so a neighbour usually tops the map.
       Not always: open water can score as high, and sampling is free to fire there. */
    clear_board(&b);
    b.st[5 * 10 + 5] = S_HIT;
    ai_clear_queue();
    {
        int chased = 0;
        for (k = 0; k < 40; k++) {
            uint8_t n = ai_admiral(&b, 500);
            CHECK(mc_samples >= MC_MIN_SAMPLES, "a lone hit leaves plenty of valid fleets");
            CHECK(mc_score[5 * 10 + 5] == 0, "a hit cell is not scored");
            CHECK(mc_score[5 * 10 + 4] + mc_score[5 * 10 + 6] + mc_score[4 * 10 + 5] + mc_score[6 * 10 + 5] >= mc_samples,
                  "every fleet runs on from the hit");
            CHECK(mc_score[4 * 10 + 4] == 0 && mc_score[6 * 10 + 6] == 0, "the moat of a hit gets no score");
            chased += n == 5 * 10 + 4 || n == 5 * 10 + 6 || n == 4 * 10 + 5 || n == 6 * 10 + 5;
        }
        printf("admiral: a lone hit was chased on %d of 40 shots\n", chased);
        CHECK(chased >= 30, "sampling chases a hit without the queue");
    }

    /* a live ship cannot be all hits: with only a sub and a cruiser afloat, a hit means cruiser */
    clear_board(&b);
    b.hits[0] = 5; b.hits[1] = 4; b.hits[3] = 2; b.hits[4] = 2; b.hits[5] = 1;      /* cruiser, one sub */
    b.st[4 * 10 + 4] = S_HIT;
    b.st[5 * 10 + 4] = S_HIT;
    mc_begin(&b);
    for (k = 0; k < 400; k++) mc_step(&b);
    CHECK(mc_samples >= MC_MIN_SAMPLES, "hits leave valid fleets");
    CHECK(mc_score[3 * 10 + 4] + mc_score[6 * 10 + 4] >= mc_samples, "two hits and a live cruiser: one end or the other, in every fleet");

    /* few valid fleets: every water cell fired, so nearly all that is left is the real fleet */
    {
        int sampled = 0, fell_back = 0;
        for (g = 0; g < 300; g++) {
            uint8_t n;
            deal(&b, g);
            for (i = 0; i < 100; i++) if (b.own[i] == CELL_EMPTY) b.st[i] = S_MISS;
            for (i = 0; i < 100; i++)
                if (b.own[i] != CELL_EMPTY && sim_rand() % 3 == 0 && b.hits[b.own[i]] + 1 < ship_len[b.own[i]]) {
                    b.st[i] = S_HIT;
                    b.hits[b.own[i]]++;
                }
            ai_clear_queue();
            n = ai_admiral(&b, 400);
            if (mc_samples >= MC_MIN_SAMPLES) sampled++; else fell_back++;
            admiral_shot_ok(&b, n, "few valid fleets");
            CHECK(b.own[n] != CELL_EMPTY, "with only hull left unfired, the shot is a hit");
        }
        printf("admiral: few valid fleets: %d sampled, %d fell back\n", sampled, fell_back);
        CHECK(sampled + fell_back == 300, "every position gave a shot");
    }

    /* few valid fleets: rare, but the sampler can reach them. Given time, it finds one. */
    for (g = 0; g < 100; g++) {
        deal(&b, g);
        for (i = 0; i < 100; i++) {
            if (b.own[i] == CELL_EMPTY) b.st[i] = S_MISS;
            else if (b.own[i] < 3) { fire(&b, (uint8_t)(i % 10), (uint8_t)(i / 10), &sunk); }
        }
        mc_begin(&b);
        for (k = 0; k < 60000 && !mc_samples; k++) mc_step(&b);
        CHECK(mc_samples > 0, "the sampler can find a fleet that fills what is left");
    }

    /* no valid fleet at all: a tight manual layout can leave a ship unplaced (place_fleet's
       START), so the carrier here is live to the AI and exists nowhere. A whole game plays out. */
    {
    int reached = 0;
    for (g = 0; g < 50; g++) {
        int n = 0;
        uint8_t zero = 0;
        deal(&b, g);
        for (i = 0; i < 100; i++) if (b.own[i] == 0) b.own[i] = CELL_EMPTY;
        b.alive = NSHIP - 1;
        ai_clear_queue();
        while (b.alive && n++ < 200) {
            uint8_t p = ai_admiral(&b, 200), r;
            if (mc_samples == 0) zero = 1;
            admiral_shot_ok(&b, p, "missing ship");
            r = fire(&b, (uint8_t)(p % 10), (uint8_t)(p / 10), &sunk);
            if (r == 1) ai_hit(&b, (uint8_t)(p % 10), (uint8_t)(p / 10));
            if (r == 2) ai_clear_queue();
        }
        CHECK(b.alive == 0, "a game with no possible fleet still finishes");
        reached += zero;
    }
    CHECK(reached > 25, "and most of them did run out of valid fleets");
    }

    /* more hits than ships could cover, and a hit next to a sunk ship: odd, and must not hang */
    clear_board(&b);
    for (y = 0; y < 10; y += 2) for (x = 0; x < 10; x += 2) b.st[y * 10 + x] = S_HIT;
    i = ai_admiral(&b, 200);
    CHECK(mc_samples == 0, "25 scattered hits have no valid fleet");
    CHECK(b.st[i] == S_EMPTY, "and the fallback still gives an unfired cell");
    clear_board(&b);
    b.st[0] = S_SUNK; b.hits[5] = 1; b.st[1] = S_HIT;
    i = ai_admiral(&b, 200);
    CHECK(mc_samples == 0 && b.st[i] == S_EMPTY, "a hit touching a sunk ship has no valid fleet, and a legal shot");

    /* the fallback rebuilds its queue: A sinks with B partly hit, and the sinking clears the queue */
    clear_board(&b);
    place_ship(&b, 0, 0, 1, 1, 5);
    place_ship(&b, 5, 5, 2, 1, 3);
    fire(&b, 0, 0, &sunk);
    fire(&b, 5, 5, &sunk);
    ai_clear_queue();
    i = ai_admiral(&b, 0);
    CHECK(i == 5 * 10 + 4 || i == 5 * 10 + 6 || i == 4 * 10 + 5 || i == 6 * 10 + 5, "the fallback finds the live hit after a sinking cleared the queue");

    /* the line is 50 valid fleets: below it the queue fires, whatever the fleets say. The queue's
       first cell is the one above the hit, and the fleets alone would pick it about one time in 4. */
    clear_board(&b);
    b.st[5 * 10 + 5] = S_HIT;
    for (k = 0; k < 40; k++) {
        mc_begin(&b);
        while (mc_samples < MC_MIN_SAMPLES - 1) mc_step(&b);
        CHECK(mc_pick(&b) == 4 * 10 + 5, "with too few fleets, the honest queue takes the shot");
    }
    CHECK(MC_MIN_SAMPLES == 50, "the fallback line is about 50 samples");

    /* a queue left from earlier hits holds no spent cell, once ADMIRAL has fired one of them */
    clear_board(&b);
    b.st[5 * 10 + 5] = S_HIT;
    ai_clear_queue();
    ai_hit(&b, 5, 5);
    i = ai_admiral(&b, MC_TRIES);
    CHECK(target[i] == 0, "the shot leaves the queue");

    /* nothing but known water left: fire there rather than hang */
    clear_board(&b);
    for (i = 0; i < 100; i++) b.st[i] = S_MISS;
    b.st[4 * 10 + 4] = S_SUNK; b.hits[5] = 1;
    b.st[4 * 10 + 5] = S_EMPTY;
    i = ai_admiral(&b, 200);
    CHECK(i == 4 * 10 + 5, "known water when nothing else is left");

    /* no samples at all, every turn: the fallback alone plays legal whole games */
    for (g = 0; g < 200; g++) {
        int n = 0;
        deal(&b, g);
        ai_clear_queue();
        while (b.alive && n++ < 200) {
            uint8_t p = ai_admiral(&b, 0), r;
            admiral_shot_ok(&b, p, "fallback game");
            r = fire(&b, (uint8_t)(p % 10), (uint8_t)(p / 10), &sunk);
            if (r == 1) ai_hit(&b, (uint8_t)(p % 10), (uint8_t)(p / 10));
            if (r == 2) ai_clear_queue();
        }
        CHECK(b.alive == 0, "the fallback alone finishes a game");
    }
}

/* Solo games at a tries cap of what the ROM reaches, on the same fleets as HARD. */
static int solo_shots(Board *b, int g, uint8_t level, uint16_t tries) {
    int n = 0;
    deal(b, g);
    ai_clear_queue();
    while (b->alive && n < 200) {
        uint8_t i = level == 3 ? ai_admiral(b, tries) : ai_next(b, level), sunk = 0;
        uint8_t r = fire(b, (uint8_t)(i % 10), (uint8_t)(i / 10), &sunk);
        n++;
        if (r == 1) ai_hit(b, (uint8_t)(i % 10), (uint8_t)(i / 10));
        if (r == 2) ai_clear_queue();
    }
    return n;
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

    segment_checks();
    blit_checks();

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
    for (t = 0; t < 4; t++) {
        int game;
        for (game = 0; game < 100; game++) sim_game(&b, t);
    }

    /* ---- the fairness test, at every level ---- */
    for (t = 0; t < 4; t++) {
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

    /* ---- ADMIRAL ---- */
    admiral_checks();
    clever_checks();

    /* strength: 2000 fleets at 500 tries a turn, the ROM's mean over the first 10 turns (its whole-
       game mean is about 870, and more tries buy nothing measurable). The fleets are fixed, not
       swept: the margin is small. Over 10 disjoint blocks of 2000 fleets ADMIRAL led in all 10, by
       0.43 to 0.78 shots. */
    {
        int game, hard_total = 0, admiral_total = 0;
        for (game = 0; game < 2000; game++) {
            hard_total += solo_shots(&b, game, 2, 0);
            admiral_total += solo_shots(&b, game, 3, 500);
        }
        printf("sim: admiral %.2f avg, hard %.2f avg (2000 fleets, 500 tries a turn)\n",
               admiral_total / 2000.0, hard_total / 2000.0);
        CHECK(admiral_total < hard_total, "ADMIRAL beats HARD on the same fleets");
    }

    printf("test_place: %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
