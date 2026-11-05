/* Naval Hunter for the GBA: the GB game's flow and rules with both boards on screen, in
 * placeholder art. Mode 4, one page. The enemy board is big, yours is small, and a board
 * only ever redraws the cells whose look changed (sync_boards).
 *
 *   y 3    status (left), YOUR WATERS (right)
 *   y 14   A-J over the enemy board at (12,22); 1-10 down its left
 *   y 12   your board at (144,12). It moved up 8px from the plan's (144,20) to make room
 *   y 95   for the ENEMY FLEET heading, so the list can still start at y 103
 *   y 103  the fleet list, 8px a line, to y 159
 *   y 145  message line, y 153 control hint (both under the big board). y 146 left no gap
 *          between the two
 */
#include <tonc.h>
#include <string.h>
#include "gfx.h"
#include "sim.h"

enum { ST_BOOT, ST_TITLE, ST_MENU, ST_PLACE, ST_BATTLE, ST_ENEMY, ST_END };
enum { C_BG, C_INK, C_GRID, C_SEA, C_SHIP, C_BAD, C_RED, C_DKRED, C_YEL, C_DIM, C_KNOWN };
enum { VIEW_HIDDEN, VIEW_SHIPS, VIEW_END };     /* what a board shows of its fleet */
enum { L_WATER, L_MISS, L_HIT, L_SUNK, L_SHIP, L_BAD, L_KNOWN, L_UNDRAWN = 0xFF };

#define BIG_X 12
#define BIG_Y 22
#define SMALL_X 144
#define SMALL_Y 12
#define PANEL_X 144
#define PANEL_Y 103
#define THINK_FRAMES 30         /* the enemy's pause; the AI runs inside it */
#define ADMIRAL 3
/* ADMIRAL samples until this scanline, which leaves the frame's own work 80 lines: the idle board
   scan's 56 lines (68,950 cycles) and the longest mc_step that may just have started. */
#define MC_STOP_LINE 80
#define REPEAT_DELAY 18         /* the GB's key repeat: ~0.30 s before it kicks in */
#define REPEAT_RATE   9         /* then ~6.7 moves a second */

static const char *const ship_name[NSHIP] = {
    "CARRIER", "BATTLESHIP", "CRUISER", "DESTROYER", "DESTROYER", "SUB", "SUB"
};
static const char *const level_name[4] = { "EASY", "NORMAL", "HARD", "ADMIRAL" };

typedef struct {
    int x, y, size;
    const Board *b;             /* NULL: not on screen */
    u8 view;
    u8 shown[100];              /* each cell's look as last drawn */
    /* is_known_water per cell, kept because 200 of its 3x3 scans cost 70% of a frame. It can
       only grow when a ship sinks, and look() checks a fired cell first. */
    u8 moat[100];
    u8 moat_alive;              /* b->alive when moat was built */
} Pane;

/* Read by tools/e2e.py through the .sym file, so not static. */
u8 state = ST_BOOT;
volatile u32 frames, frame_drops;
u32 game_seed;                  /* this game's sim_seed, so e2e can deal the same fleets itself */
volatile u32 ai_cycles;         /* the slowest ai_next or mc_pick so far, in CPU cycles */
volatile u32 worst_frame;       /* the most cycles a frame's work has taken so far */
u16 samples, sample_tries;      /* ADMIRAL's last turn: valid fleets, and fleets tried */
u8 think_frames;                /* frames the enemy's last turn spent thinking */
volatile u32 cells_drawn;
Board pb, ob;                   /* your waters, enemy waters */
u8 ai_level = 1;
u8 cx, cy;
u8 place_s, place_x, place_y, place_horiz;
u8 last_enemy;
u16 shots_fired, shots_hit;
char status_text[24], msg_text[24];

static Pane big = { BIG_X, BIG_Y, 12, NULL, VIEW_HIDDEN, {0}, {0}, 0xFF };
static Pane mini = { SMALL_X, SMALL_Y, 8, NULL, VIEW_SHIPS, {0}, {0}, 0xFF };
static u8 preview_len, preview_legal;   /* the ship in hand during placement; 0 = none */
static OBJ_ATTR obuf[2];                /* 0 the aiming cursor, 1 the enemy's last shot */
static volatile u8 work_done = 1, thinking;
static u8 held_btn, held_timer;
static u32 frame_start;

/* ---------------------------------------------------------------- the boards */
static u8 look(const Pane *p, int i)
{
    int st = p->b->st[i];
    if (st == S_SUNK) return L_SUNK;
    if (st == S_HIT) return L_HIT;
    if (st == S_MISS && p->view != VIEW_END) return L_MISS;     /* the end screen hides misses, as on GB */
    if (p->view != VIEW_HIDDEN && p->b->own[i] != CELL_EMPTY) return L_SHIP;
    if (p->view != VIEW_END && p->moat[i]) return L_KNOWN;
    return L_WATER;
}

static void draw_cell(const Pane *p, int i, int l)
{
    static const u8 bg[] = { C_SEA, C_SEA, C_RED, C_DKRED, C_SHIP, C_BAD, C_KNOWN };
    int n = p->size - 1, x = p->x + i % 10 * p->size + 1, y = p->y + i / 10 * p->size + 1;
    int w = l == L_MISS ? 3 : 5;
    gfx_rect(x, y, n, n, bg[l]);
    /* On the big board the 3px mark runs odd x to odd x and the 5px one even to even, so
       tools/e2e.py can pin a pixel written after its halfword neighbour, both ways round. */
    if (l == L_MISS || l == L_HIT || l == L_SUNK)
        gfx_rect(x + (n - w) / 2, y + (n - w) / 2, w, w, l == L_MISS ? C_INK : l == L_HIT ? C_YEL : C_RED);
    cells_drawn++;
}

static void sync_pane(Pane *p)
{
    int i;
    if (p->b && p->b->alive != p->moat_alive) {
        /* About 80 scanlines. Start only if they fit before VBlank, else carry on next frame. */
        if (REG_VCOUNT >= 60 && REG_VCOUNT < 160) return;
        for (i = 0; i < 100; i++) p->moat[i] = is_known_water(p->b, i % 10, i / 10);
        p->moat_alive = p->b->alive;
    }
    for (i = 0; p->b && i < 100; i++) {
        int want = look(p, i);
        if (p == &big && preview_len) {
            int dx = i % 10 - place_x, dy = i / 10 - place_y;
            if (place_horiz ? dy == 0 && dx >= 0 && dx < preview_len
                            : dx == 0 && dy >= 0 && dy < preview_len)
                want = preview_legal ? L_SHIP : L_BAD;
        }
        if (want == p->shown[i]) continue;
        /* A whole board does not fit in a frame. Stop near the end of this one and carry on
           in the next: a cell costs a few scanlines at most. */
        if (REG_VCOUNT >= 140 && REG_VCOUNT < 160) return;
        draw_cell(p, i, want);
        p->shown[i] = want;
    }
}

static void sync_boards(void)
{
    sync_pane(&big);
    sync_pane(&mini);
}

/* ---------------------------------------------------------------- frame */
static void on_vbl(void)
{
    frames++;
    if (!work_done && !thinking) frame_drops++;  /* this VBlank came before the frame's work ended */
    work_done = 0;
}

/* Timers 2 and 3 cascaded: a free-running 32-bit count of CPU cycles. Low half read between two
   reads of the high half, or a carry between the reads puts it 65,536 out. */
static u32 cycles(void)
{
    u32 hi, lo;
    do { hi = REG_TM3D; lo = REG_TM2D; } while (hi != REG_TM3D);
    return hi << 16 | lo;
}

/* The one place the game waits. */
static void wait_frame(void)
{
    u32 cost;
    sync_boards();
    cost = cycles() - frame_start;
    if (cost > worst_frame) worst_frame = cost;
    work_done = 1;
    VBlankIntrWait();
    frame_start = cycles();
    oam_copy(oam_mem, obuf, 2);
    key_poll();
}

static void pause_frames(int n) { while (n--) wait_frame(); }

/* ---------------------------------------------------------------- text */
static void text_row(int x, int y, int w, const char *s, int c)
{
    gfx_rect(x, y, w, 7, C_BG);
    gfx_text(x, y, s, c);
}

static void text_mid(int y, const char *s, int c)
{
    gfx_text((GFX_W - 6 * (int)strlen(s)) / 2, y, s, c);
}

static void status(const char *s)
{
    if (strcmp(s, status_text)) { strcpy(status_text, s); text_row(BIG_X, 3, 126, s, C_INK); }
}

static void msg(const char *s)
{
    if (strcmp(s, msg_text)) { strcpy(msg_text, s); text_row(BIG_X, 145, 126, s, C_YEL); }
}

static void hint(const char *s) { text_row(BIG_X, 153, 126, s, C_DIM); }

/* Forget everything on screen. A DMA-speed fill: gfx_rect takes several frames over this. */
static void clear_screen(void)
{
    big.b = mini.b = NULL;
    status_text[0] = msg_text[0] = 0;
    memset32((void *)gfx_fb, 0, GFX_W * GFX_H / 4);
}

/* The grid, and every cell owed a draw once the caller sets p->b. */
static void pane_grid(Pane *p, int view)
{
    int k;
    for (k = 0; k <= 10; k++) {
        gfx_rect(p->x + k * p->size, p->y, 1, 10 * p->size + 1, C_GRID);
        gfx_rect(p->x, p->y + k * p->size, 10 * p->size + 1, 1, C_GRID);
    }
    p->view = view;
    p->moat_alive = 0xFF;
    memset(p->shown, L_UNDRAWN, sizeof p->shown);
}

static void big_labels(void)
{
    int k;
    for (k = 0; k < 10; k++) {
        char c[3] = { (char)('A' + k), 0, 0 };
        gfx_text(BIG_X + 12 * k + 4, BIG_Y - 8, c, C_DIM);
        if (k < 9) c[0] = (char)('1' + k);
        else { c[0] = '1'; c[1] = '0'; }
        gfx_text(k < 9 ? 5 : 0, BIG_Y + 12 * k + 3, c, C_DIM);
    }
}

/* One line of the fleet list. A sunk ship is red and struck through; a ship not on the
   board yet is blank, so during placement the list is progress, not a promise. */
static void panel_line(const Board *b, int s)
{
    int y = PANEL_Y + 8 * s, sunk = b->hits[s] >= ship_len[s], c = sunk ? C_RED : C_INK, k;
    gfx_rect(PANEL_X, y, 92, 7, C_BG);
    if (!ship_is_placed(b, s)) return;
    gfx_text(PANEL_X, y, ship_name[s], c);
    for (k = 0; k < ship_len[s]; k++) gfx_rect(PANEL_X + 64 + 5 * k, y + 1, 4, 5, c);
    if (sunk) gfx_rect(PANEL_X, y + 3, 89, 1, c);
}

/* ---------------------------------------------------------------- sound */
/* The GB's channel 4 and channel 1 values: on the GBA the envelope is the high byte of
   SND4CNT/SND1CNT, the noise clock the low byte of SND4FREQ, and the note codes are the same. */
static void sound_init(void)
{
    REG_SNDSTAT = 0x80;         /* PSG on (must be first) */
    REG_SNDDMGCNT = 0xFF77;     /* all channels both sides, max volume */
    REG_SNDDSCNT = 2;           /* DMG mix 100% */
}

static void sound_explosion(int big_one)
{
    REG_SND4CNT = big_one ? 0xF700 : 0xF300;    /* a ship going down decays slower... */
    REG_SND4FREQ = 0x8000 | (big_one ? 0x76 : 0x65);   /* ...and is lower */
}

static void sound_tick(void)
{
    REG_SND4CNT = 0x2100;
    REG_SND4FREQ = 0x8071;
}

static const u16 jingle_win[4] = { 1797, 1849, 1881, 1922 };    /* C5 E5 G5 C6 */
static const u16 jingle_lose[4] = { 1797, 1713, 1650, 1546 };   /* C5 G4 E4 C4 */

static void jingle(const u16 *notes)
{
    int i;
    REG_SND1SWEEP = 0;
    REG_SND1CNT = 0x7380;       /* 50% duty, vol 7, decay */
    for (i = 0; i < 4; i++) {
        REG_SND1FREQ = 0x8000 | notes[i];
        pause_frames(9);
    }
    REG_SND1CNT = 0;
}

static void flash_hit(void)
{
    REG_BLDY = 8;
    pause_frames(8);
    REG_BLDY = 0;
}

/* ---------------------------------------------------------------- input */
/* The direction to step this frame: at once on a press, then the GB's repeat. */
static int repeat_dir(void)
{
    int d = key_curr_state() & KEY_DIR;
    if (!d) { held_btn = 0; return 0; }
    if (d != held_btn) { held_btn = d; held_timer = REPEAT_DELAY; return d; }
    if (held_timer) { held_timer--; return 0; }
    held_timer = REPEAT_RATE;
    return d;
}

/* ---------------------------------------------------------------- screens */
static void title_screen(void)
{
    int f = 0;
    state = ST_TITLE;
    text_mid(40, "NAVAL HUNTER", C_INK);
    for (;;) {
        /* Only START leaves. Everything else is ignored, so mashing cannot start a game. */
        if (f % 120 == 0) text_mid(100, "PRESS START", C_YEL);
        if (f % 120 == 60) gfx_rect(0, 100, GFX_W, 7, C_BG);
        wait_frame();
        if (key_hit(KEY_START)) return;
        f++;
    }
}

static void menu_level(void)
{
    char b[24] = "SELECT LEVEL: ";
    gfx_rect(0, 120, GFX_W, 7, C_BG);
    text_mid(120, strcat(b, level_name[ai_level]), C_YEL);
}

/* Returns 1 for manual placement. Key edges, so a held button cannot skip this screen. */
static int menu(void)
{
    state = ST_MENU;
    clear_screen();
    wait_frame();
    text_mid(40, "NAVAL HUNTER", C_INK);
    text_mid(70, "PLACE YOUR FLEET?", C_INK);
    text_mid(90, "A RANDOM   B MANUAL", C_INK);
    menu_level();
    for (;;) {
        wait_frame();
        if (key_hit(KEY_A)) return 0;
        if (key_hit(KEY_B)) return 1;
        if (key_hit(KEY_SELECT)) {
            ai_level = (ai_level + 1) % 4;
            menu_level();
        }
    }
}

static void place_fleet(void)
{
    state = ST_PLACE;
    clear_board(&pb);
    clear_screen();
    wait_frame();
    place_s = place_x = place_y = 0;
    place_horiz = 1;
    pane_grid(&big, VIEW_SHIPS);
    big.b = &pb;
    status("PLACE YOUR SHIPS");
    wait_frame();
    big_labels();
    gfx_text(PANEL_X, PANEL_Y - 8, "YOUR FLEET", C_DIM);
    hint("A OK B TURN ST RANDOM");
    while (place_s < NSHIP) {
        int len = ship_len[place_s], d;
        int maxx = place_horiz ? 10 - len : 9, maxy = place_horiz ? 9 : 10 - len;
        char b[24] = "1/7 ";

        /* keep the whole hull on the board after a rotate */
        if (place_x > maxx) place_x = maxx;
        if (place_y > maxy) place_y = maxy;
        preview_len = len;
        preview_legal = can_place(&pb, place_x, place_y, len, place_horiz);
        b[0] = (char)('1' + place_s);
        strcat(b, ship_name[place_s]);
        strcat(b, preview_legal ? " OK" : " NO FIT");
        msg(b);
        wait_frame();

        if (key_hit(KEY_START)) {       /* randomise the rest */
            for (; place_s < NSHIP; place_s++) {
                /* Failing is very unlikely, after a tight manual layout. The list then shows
                   a ship short, and alive must agree or that ship could never be sunk. */
                if (!place_random_one(&pb, place_s)) pb.alive--;
                panel_line(&pb, place_s);
            }
            break;
        }
        if (key_hit(KEY_B)) { place_horiz ^= 1; continue; }
        if (key_hit(KEY_A)) {
            if (preview_legal) {
                place_ship(&pb, place_x, place_y, len, place_horiz, place_s);
                panel_line(&pb, place_s);
                place_s++;
                place_x = place_y = 0;
            } else {
                msg("TOO CLOSE");
                pause_frames(30);
            }
            continue;
        }
        d = repeat_dir();
        if ((d & KEY_LEFT) && place_x > 0) place_x--;
        if ((d & KEY_RIGHT) && place_x < maxx) place_x++;
        if ((d & KEY_UP) && place_y > 0) place_y--;
        if ((d & KEY_DOWN) && place_y < maxy) place_y++;
    }
    preview_len = 0;
}

static void cursor_show(void)
{
    obj_set_pos(&obuf[0], BIG_X + 12 * cx, BIG_Y + 12 * cy);
    obj_unhide(&obuf[0], 0);
}

/* Message, sound and flash for one resolved shot. who is "" for yours, "YOUR " for theirs. */
static void shot_result(int r, int sunk, const char *who, const char *miss, const char *hit)
{
    char b[24];
    if (r == 0) { msg(miss); sound_tick(); return; }
    if (r == 1) { msg(hit); sound_explosion(0); }
    else {
        strcpy(b, who);
        strcat(b, ship_name[sunk]);
        msg(strcat(b, " SUNK"));
        sound_explosion(1);
    }
    flash_hit();
}

/* ADMIRAL samples through the whole pause, a frame's worth at a time, so the frame loop keeps
   running and its frames count like any others. The shot is picked at the top of the frame after. */
static int admiral_think(void)
{
    u32 t0 = frames, c;
    int i;
    mc_begin(&pb);
    while (frames - t0 < THINK_FRAMES) {
        while (REG_VCOUNT >= 160 || REG_VCOUNT < MC_STOP_LINE) mc_step(&pb);
        wait_frame();
    }
    think_frames = frames - t0;
    samples = mc_samples;
    sample_tries = mc_tries;
    c = cycles();
    i = mc_pick(&pb);
    c = cycles() - c;
    if (c > ai_cycles) ai_cycles = c;
    return i;
}

/* ai_next at HARD takes several frames in Thumb from ROM (tools/e2e.py measures it and fails
   if it outgrows the pause). It runs inside the enemy's pause, where nothing on screen is
   moving, so it does not count as a dropped frame. The VBlank IRQ still runs throughout. */
static int enemy_think(void)
{
    u32 t0 = frames, c = cycles();
    int i;
    thinking = 1;
    i = ai_next(&pb, ai_level);
    c = cycles() - c;
    if (c > ai_cycles) ai_cycles = c;
    wait_frame();
    thinking = 0;
    while (frames - t0 < THINK_FRAMES) wait_frame();
    think_frames = frames - t0;
    return i;
}

static void enemy_turn(void)
{
    int i, x, y, r;
    u8 sunk = 0;
    status("ENEMY TURN");
    obj_hide(&obuf[0]);
    i = ai_level == ADMIRAL ? admiral_think() : enemy_think();
    x = i % 10; y = i / 10;
    r = fire(&pb, x, y, &sunk);
    if (r == 3) {
        /* ai_next should never pick a spent square. If it does, it must not stall the game. */
        for (i = 0; i < 100 && pb.st[i] != S_EMPTY; i++) ;
        x = i % 10; y = i / 10;
        r = fire(&pb, x, y, &sunk);
    }
    if (r == 1) ai_hit(&pb, x, y);
    if (r == 2) ai_clear_queue();
    last_enemy = y * 10 + x;
    obj_set_pos(&obuf[1], SMALL_X + 8 * x, SMALL_Y + 8 * y);
    obj_unhide(&obuf[1], 0);
    shot_result(r, sunk, "YOUR ", "ENEMY MISS", "ENEMY HIT!");
}

/* One game's shooting. Returns 1 if you won. */
static int battle(void)
{
    state = ST_BATTLE;
    clear_screen();
    wait_frame();
    cx = cy = 0;
    held_btn = held_timer = 0;
    shots_fired = shots_hit = 0;
    last_enemy = 0xFF;
    /* The chrome takes several frames, a piece a frame, and then the boards paint in. */
    pane_grid(&big, VIEW_HIDDEN);
    wait_frame();
    pane_grid(&mini, VIEW_SHIPS);
    wait_frame();
    big_labels();
    wait_frame();
    {
        int s;
        for (s = 0; s < NSHIP; s++) {
            panel_line(&ob, s);
            wait_frame();
        }
    }
    big.b = &ob;
    mini.b = &pb;
    gfx_text(SMALL_X, 3, "YOUR WATERS", C_DIM);
    gfx_text(PANEL_X, PANEL_Y - 8, "ENEMY FLEET", C_DIM);
    hint("D-PAD AIM   A FIRE");
    status("YOUR TURN");
    cursor_show();

    for (;;) {
        int d, r;
        u8 sunk = 0;
        wait_frame();
        d = repeat_dir();
        if (d & KEY_LEFT) cx = cx ? cx - 1 : 9;
        if (d & KEY_RIGHT) cx = cx < 9 ? cx + 1 : 0;
        if (d & KEY_UP) cy = cy ? cy - 1 : 9;
        if (d & KEY_DOWN) cy = cy < 9 ? cy + 1 : 0;
        cursor_show();
        if (!key_hit(KEY_A)) continue;

        /* a spent square or known water is refused and costs no turn */
        r = is_known_water(&ob, cx, cy) ? 4 : fire(&ob, cx, cy, &sunk);
        if (r >= 3) {
            msg(r == 3 ? "ALREADY SHOT" : "KNOWN WATER");
            pause_frames(30);
            msg("");
            continue;
        }
        state = ST_ENEMY;
        shots_fired++;
        if (r) shots_hit++;
        if (r == 2) panel_line(&ob, sunk);
        shot_result(r, sunk, "", "MISS", "HIT!");
        if (ob.alive == 0) return 1;
        pause_frames(45);

        enemy_turn();
        if (pb.alive == 0) return 0;
        pause_frames(45);

        state = ST_BATTLE;
        status("YOUR TURN");
        msg("");
    }
}

static int fmt_u16(char *p, int v)
{
    char d[5];
    int n = 0, m = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) p[m++] = d[--n];
    return m;
}

/* Both fleets revealed in place, so only the cells that change get redrawn. */
static void end_screen(int won)
{
    char b[24];
    int n;
    jingle(won ? jingle_win : jingle_lose);
    state = ST_END;
    obj_hide(&obuf[0]);
    obj_hide(&obuf[1]);
    big.view = mini.view = VIEW_END;
    status(won ? "YOU WIN!" : "YOU LOSE");
    n = fmt_u16(b, shots_fired);
    strcpy(b + n, " SHOTS ");
    n += 7;
    n += fmt_u16(b + n, shots_fired ? shots_hit * 100 / shots_fired : 0);
    strcpy(b + n, "%");
    msg(b);
    hint("START NEW GAME");
    do wait_frame(); while (!key_hit(KEY_START));
}

/* ---------------------------------------------------------------- boot */
/* A hollow n x n box in a sprite's tiles, w tiles wide. */
static void make_box(TILE *t, int w, int n, u32 c)
{
    int x, y;
    for (y = 0; y < n; y++)
        for (x = 0; x < n; x++)
            if (x == 0 || y == 0 || x == n - 1 || y == n - 1)
                t[y / 8 * w + x / 8].data[y % 8] |= c << (x % 8 * 4);
}

int main(void)
{
    REG_WAITCNT = WS_STANDARD;
    oam_init(oam_mem, 128);
    oam_init(obuf, 2);
    pal_bg_mem[C_BG] = RGB15(1, 3, 7);
    pal_bg_mem[C_INK] = RGB15(31, 31, 31);
    pal_bg_mem[C_GRID] = RGB15(6, 12, 20);
    pal_bg_mem[C_SEA] = RGB15(3, 8, 16);
    pal_bg_mem[C_SHIP] = RGB15(20, 22, 24);
    pal_bg_mem[C_BAD] = RGB15(31, 12, 0);
    pal_bg_mem[C_RED] = RGB15(29, 4, 4);
    pal_bg_mem[C_DKRED] = RGB15(10, 1, 2);
    pal_bg_mem[C_YEL] = RGB15(31, 31, 4);
    pal_bg_mem[C_DIM] = RGB15(16, 20, 24);
    pal_bg_mem[C_KNOWN] = RGB15(10, 18, 26);
    pal_obj_mem[1] = RGB15(31, 31, 4);
    pal_obj_mem[2] = RGB15(31, 31, 31);
    /* Mode 4 keeps sprite tiles from index 512: lower ones are hidden by the bitmap. */
    make_box(&tile_mem[5][0], 2, 13, 1);
    make_box(&tile_mem[5][4], 1, 8, 2);
    obj_set_attr(&obuf[0], ATTR0_SQUARE | ATTR0_HIDE, ATTR1_SIZE_16, 512);
    obj_set_attr(&obuf[1], ATTR0_SQUARE | ATTR0_HIDE, ATTR1_SIZE_8, 516);
    REG_TM2D = REG_TM3D = 0;
    REG_TM3CNT = TM_CASCADE | TM_ENABLE;
    REG_TM2CNT = TM_ENABLE;
    REG_BLDCNT = BLD_TOP_MASK | BLD_WHITE;      /* every layer and the backdrop towards white */
    sound_init();
    gfx_fb = (volatile u16 *)MEM_VRAM;
    clear_screen();
    REG_DISPCNT = DCNT_MODE4 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
    irq_init(NULL);
    irq_add(II_VBLANK, on_vbl);

    title_screen();             /* outside the loop: a replay goes straight to the menu */
    for (;;) {
        int manual = menu();
        /* After the keypress, per game: human timing is the only entropy here, and seeding
           at boot deals the same fleets every power-on (the GB game had this bug). */
        game_seed = frames * 0x9E3779B1u ^ cycles();
        sim_seed(game_seed);
        if (ai_level == ADMIRAL) place_fleet_clever(&ob, wait_frame);
        else init_board(&ob);
        if (manual) place_fleet();
        else init_board(&pb);
        ai_clear_queue();
        end_screen(battle());
    }
}
