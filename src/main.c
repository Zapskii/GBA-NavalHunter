/* Naval Hunter for the GBA: the GB game's flow and rules with both boards on screen. Mode 4, one
 * page. tools/gen_art.py draws the art and its top comment gives the palette plan. The enemy board is big, yours is small, and a board
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
#include "art.h"
#include "gfx.h"
#include "sim.h"

enum { ST_BOOT, ST_TITLE, ST_MENU, ST_PLACE, ST_BATTLE, ST_ENEMY, ST_END, ST_SPLASH };
enum { VIEW_HIDDEN, VIEW_SHIPS, VIEW_END };     /* what a board shows of its fleet */
#define UNDRAWN 0xFF            /* in shown[]: no art from art.h's T_* */

#define BIG_X 12
#define BIG_Y 22
#define SMALL_X 144
#define SMALL_Y 12
#define PANEL_X 144
#define PANEL_Y 103
#define ICON_X (PANEL_X + 63)   /* odd, so tools/e2e.py pins gfx_blit's odd-x path on a real pixel */
#define START_Y 133             /* the PRESS START on the title */
#define SPLASH_FRAMES 300       /* the win or lose art's hold, as on GB: 5 seconds */
#define SPLASH_LOCK 36          /* no skipping at first, so mashing the last shot's A cannot throw the picture away. A later skip cuts the jingle */
#define THINK_FRAMES 30         /* the enemy's pause; the AI runs inside it */
#define SEA_RATE 8              /* frames per step of the sea's 16: a wave passes in about 2 seconds */
#define SHELL_FRAMES 24         /* a shell's flight */
#define IMPACT_RATE 4           /* frames per frame of splash or blast art */
#define SINK_STAGGER 3          /* frames between a sinking ship's blasts, one per hull cell */
#define AFTER_SHOT 6            /* a beat after the impact */
#define FLASH_FRAMES 8
#define SHAKE_FRAMES 16
/* Fixed points, so a shell's start says nothing: your shells rise from your board, theirs from theirs. */
#define YOUR_GUN_X 184
#define YOUR_GUN_Y 52
#define THEIR_GUN_X 72
#define THEIR_GUN_Y 82
#define ADMIRAL 3
/* ADMIRAL samples until this scanline, which leaves the frame's own work 72 lines: the idle board scan's
   22 lines and the longest mc_step that may just have started. Moved from 80 to win back what the
   animation list (8 lines at the top of every frame) took from the samples. */
#define MC_STOP_LINE 88
/* Of a frame's 280,896 cycles: when a board stops and carries on in the next. It leaves room for the
   scan of up to 100 cells that can follow the last cell drawn, about 14,000, and one more cell. */
#define PAINT_LATEST 248000
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
    u8 shown[100];              /* each cell's art (T_*) as last drawn */
    /* is_known_water per cell, kept because 200 of its 3x3 scans cost 70% of a frame. It can
       only grow when a ship sinks, and look() checks a fired cell first. */
    u8 moat[100];
    u8 moat_alive;              /* b->alive when moat was built */
    u8 moat_next;               /* how far a rebuild has got */
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
volatile u32 jingle_notes;      /* notes started on any channel, ever */
volatile u32 music_cycles;      /* the most cycles music_step has taken in the VBlank handler */
volatile u16 sfx_left;          /* frames until the sample playing stops; 0 = none */
u8 sfx_cur;                     /* the last sample started */
volatile u32 sfx_starts;        /* samples started, ever */
Board pb, ob;                   /* your waters, enemy waters */
u8 ai_level = 1;
u8 cx, cy;
u8 place_s, place_x, place_y, place_horiz;
u8 last_enemy;
u16 shots_fired, shots_hit;
char status_text[24], msg_text[24];

static Pane big = { BIG_X, BIG_Y, 12, NULL, VIEW_HIDDEN, {0}, {0}, 0xFF, 0 };
static Pane mini = { SMALL_X, SMALL_Y, 8, NULL, VIEW_SHIPS, {0}, {0}, 0xFF, 0 };
static u8 preview_len, preview_legal;   /* the ship in hand during placement; 0 = none */
/* The sprites: 0 the aiming cursor, 1 the enemy's last shot, then one per entry of anim[], then smoke.
   A shell's flight scales through affine matrix 0. An impact uses matrix 1 on the small board, which
   shrinks it, and matrix 2, the identity, on the big one.
   At most 18 damaged cells a board, so 46 of the 128 are ever used. */
#define ANIMS 8
#define OBJ_ANIM 2
#define OBJ_SMOKE_AT (OBJ_ANIM + ANIMS)
#define SMOKE_MAX 36
#define OBJ_USED (OBJ_SMOKE_AT + SMOKE_MAX)
#define OBJ_ART (512 + 8)       /* where art_obj sits: after make_box's two boxes */
static OBJ_ATTR obuf[OBJ_USED];
enum { A_NONE, A_SHELL, A_SPLASH, A_BLAST, A_FLASH, A_SHAKE, A_WAIT };
/* What animates. Nothing here reads own[] of an unsunk ship or calls sim_rand, and the shot it shows
   was resolved before it started, so it can't change an outcome. t < 0: still waiting to start. */
typedef struct { u8 kind, small; s16 t, len, x, y, x0, y0; } Anim;
Anim anim[ANIMS];
u8 smoke_n;                     /* damaged cells on show */
u8 smoke_at[SMOKE_MAX];         /* each one's cell, plus 100 on your board */
static u8 smoke_x[SMOKE_MAX], smoke_y[SMOKE_MAX];
static s8 shake_x, shake_y;
static const Pane *held_pane;   /* a shell is on its way here: the board shows the shot when it lands */
u8 sea_rate = SEA_RATE;         /* a knob, and tools/e2e.py stops the sea with it to compare whole screens */
u8 sea_phase;
static u8 sea_tick;
volatile u8 shaking;            /* the backdrop shows at the picture's edge, so it takes the chrome's colour */
static volatile u8 work_done = 1;
static u32 ai_chunk;            /* where the current slice of the AI's work began */
static u8 held_btn, held_timer;
static u32 frame_start;

/* Timers 2 and 3 cascaded: a free-running 32-bit count of CPU cycles. Low half read between two
   reads of the high half, or a carry between the reads puts it 65,536 out. */
static u32 cycles(void)
{
    u32 hi, lo;
    do { hi = REG_TM3D; lo = REG_TM2D; } while (hi != REG_TM3D);
    return hi << 16 | lo;
}

/* ---------------------------------------------------------------- the boards */
static u8 look(const Pane *p, int i)
{
    const Board *b = p->b;
    int st = b->st[i];
    if (st == S_SUNK) return T_SUNK + gfx_segment(b->own, i);
    /* A hit you cannot see the ship of is a mark on water: a hull here would give its axis away. */
    if (st == S_HIT) return p->view == VIEW_HIDDEN ? T_HIT : T_HULLHIT + gfx_segment(b->own, i);
    if (st == S_MISS && p->view != VIEW_END) return T_MISS;     /* the end screen hides misses, as on GB */
    if (p->view != VIEW_HIDDEN && b->own[i] != CELL_EMPTY) return T_HULL + gfx_segment(b->own, i);
    if (p->view != VIEW_END && p->moat[i]) return T_KNOWN;
    return T_WATER;
}

/* A tile holds its cell's top and left grid lines, so a hull can run over them. */
static void draw_cell(const Pane *p, int i, int t)
{
    gfx_blit(p->x + i % 10 * p->size, p->y + i / 10 * p->size, p->size, p->size,
             p->size == 12 ? art_big[t] : art_mini[t], p->size);
    cells_drawn++;
}

static void sync_pane(Pane *p)
{
    int i;
    /* All by the clock, not REG_VCOUNT: a frame's work starts in VBlank, so a line number can't tell
       its start from an overrun. What doesn't fit carries on in the next frame. */
    if (cycles() - frame_start > PAINT_LATEST) return;
    if (p->b && p->b->alive != p->moat_alive) {
        /* Most of 100 scanlines, on top of whatever the frame has drawn already. */
        for (; p->moat_next < 100; p->moat_next++) {
            if (cycles() - frame_start > PAINT_LATEST) return;
            p->moat[p->moat_next] = is_known_water(p->b, p->moat_next % 10, p->moat_next / 10);
        }
        p->moat_next = 0;
        p->moat_alive = p->b->alive;
    }
    for (i = 0; p->b && i < 100; i++) {
        int want = look(p, i);
        if (p == &big && preview_len) {
            int dx = i % 10 - place_x, dy = i / 10 - place_y;
            if (place_horiz ? dy == 0 && dx >= 0 && dx < preview_len
                            : dx == 0 && dy >= 0 && dy < preview_len)
                want = (preview_legal ? T_HULL : T_BAD) + gfx_seg_of(place_horiz ? dx : dy, preview_len, place_horiz);
        }
        if (want == p->shown[i]) continue;
        /* A whole board does not fit in a frame. A cell costs a few scanlines at most. */
        if (cycles() - frame_start > PAINT_LATEST) return;
        draw_cell(p, i, want);
        p->shown[i] = want;
    }
}

static void sync_boards(void)
{
    if (held_pane != &big) sync_pane(&big);
    if (held_pane != &mini) sync_pane(&mini);
}

/* ---------------------------------------------------------------- frame */
static void music_step(void);
static void sfx_stop(void);

static void on_vbl(void)
{
    int k;
    u32 c;
    frames++;
    if (!work_done) frame_drops++;  /* this VBlank came before the frame's work ended */
    work_done = 0;
    /* Only palette entries 0-15, which only water uses, and only here. A fade or a flash is brightness
       and art_load is 64 up, so nothing else touches them. Minus, so the crests roll towards you. */
    if (sea_rate && ++sea_tick >= sea_rate) { sea_tick = 0; sea_phase--; }
    for (k = 0; k < 16; k++) pal_bg_mem[k] = art_shared_pal[(k + sea_phase) & 15];
    /* ponytail: index 0 is also the backdrop, so one step of water goes dark for the length of a shake.
       Remap the sea off index 0 if it ever shows. */
    if (shaking) pal_bg_mem[0] = art_shared_pal[C_BG];
    c = cycles();
    music_step();
    c = cycles() - c;
    if (c > music_cycles) music_cycles = c;
    if (sfx_left && !--sfx_left) sfx_stop();
}

/* ---------------------------------------------------------------- animation */
static void wait_frame(void);

/* Which cells smoke: S_HIT cells, read from st[] alone. The position and the loop's phase come from the
   cell's number, so no sprite can give away a ship's axis or an unhit cell. */
static void smoke_scan(void)
{
    const Pane *const panes[2] = { &big, &mini };
    int n = 0, q, i;
    for (q = 0; q < 2; q++) {
        const Pane *p = panes[q];
        if (!p->b || p->view == VIEW_END) continue;
        for (i = 0; i < 100 && n < SMOKE_MAX; i++)
            if (p->b->st[i] == S_HIT) {
                smoke_at[n] = i + 100 * q;
                smoke_x[n] = p->x + i % 10 * p->size + (p->size - 8) / 2 + 1;
                smoke_y[n] = p->y + i / 10 * p->size + (p->size - 8) / 2;
                n++;
            }
    }
    smoke_n = n;
}

static void anim_sprite(OBJ_ATTR *o, int cx, int cy, int matrix, int tile)
{
    obj_set_attr(o, ATTR0_SQUARE | ATTR0_AFF | ATTR0_Y(cy - 8 - shake_y),
                 ATTR1_SIZE_16 | ATTR1_AFF_ID(matrix) | ATTR1_X(cx - 8 - shake_x), ATTR2_ID(OBJ_ART + tile));
}

/* Shell: along a straight line, lifted into an arc, biggest at the top so it falls towards the sea. */
static void shell_step(const Anim *a, OBJ_ATTR *o)
{
    int hump = 4 * a->t * (a->len - a->t), span = a->len * a->len;
    int scale = 150 + hump * 280 / span;        /* 256 is the art's own size */
    OBJ_AFFINE *m = (OBJ_AFFINE *)obuf;
    m->pa = m->pd = (1 << 16) / scale;
    m->pb = m->pc = 0;
    anim_sprite(o, a->x0 + (a->x - a->x0) * a->t / a->len,
                a->y0 + (a->y - a->y0) * a->t / a->len - 40 * hump / span, 0, OBJ_SHELL);
}

static void shake_stop(void)
{
    shaking = shake_x = shake_y = 0;
    REG_BG2X = REG_BG2Y = 0;
}

/* From wait_frame, at the top of VBlank: registers change between pictures, and OAM is copied next. */
static void anim_step(void)
{
    int k;
    for (k = 0; k < ANIMS; k++) {
        Anim *a = &anim[k];
        OBJ_ATTR *o = &obuf[OBJ_ANIM + k];
        obj_hide(o);
        if (!a->kind || ++a->t < 0) continue;
        if (a->t >= a->len) {
            if (a->kind == A_FLASH) REG_BLDY = 0;
            if (a->kind == A_SHAKE) shake_stop();
            a->kind = A_NONE;
            continue;
        }
        switch (a->kind) {
        case A_SHELL: shell_step(a, o); break;
        case A_SPLASH: anim_sprite(o, a->x, a->y, a->small ? 1 : 2, OBJ_SPLASH + 4 * (a->t / IMPACT_RATE)); break;
        case A_BLAST: anim_sprite(o, a->x, a->y, a->small ? 1 : 2, OBJ_BLAST + 4 * (a->t / IMPACT_RATE)); break;
        case A_FLASH: REG_BLDY = a->x * (a->len - a->t) / a->len; break;
        case A_SHAKE: {
            int amp = 1 + 2 * (a->len - a->t) / a->len;
            shake_x = a->t & 2 ? amp : -amp;
            shake_y = (a->t + 1) & 2 ? amp : -amp;
            REG_BG2X = shake_x << 8;
            REG_BG2Y = shake_y << 8;
            break;
        }
        }
    }
    if (!held_pane && !(frames & 7)) smoke_scan();
    for (k = 0; k < SMOKE_MAX; k++) {
        OBJ_ATTR *o = &obuf[OBJ_SMOKE_AT + k];
        if (k >= smoke_n) { obj_hide(o); continue; }
        obj_set_attr(o, ATTR0_SQUARE | ATTR0_Y(smoke_y[k] - shake_y), ATTR1_SIZE_8 | ATTR1_X(smoke_x[k] - shake_x),
                     ATTR2_ID(OBJ_ART + OBJ_SMOKE + ((frames >> 3) + smoke_at[k] * 3) % OBJ_SMOKE_N));
    }
}

static void anim_add(int kind, int delay, int len, int x, int y, int small)
{
    int k;
    for (k = 0; k < ANIMS; k++)
        if (!anim[k].kind) {
            Anim a = { (u8)kind, (u8)small, (s16)(-delay - 1), (s16)len, (s16)x, (s16)y, 0, 0 };
            anim[k] = a;
            if (kind == A_SHAKE) shaking = 1;
            return;
        }
}

/* Everything to its end at once. Called only straight after wait_frame, so still in VBlank. */
static void anim_finish(void)
{
    int k;
    for (k = 0; k < ANIMS; k++) anim[k].kind = A_NONE;
    REG_BLDY = 0;
    shake_stop();
}

/* The one wait with a way out: a fresh A or B ends the list. key_hit is an edge, so that press is used
   up here and can't fire the next shot, and a button held down never skips anything. 1 if skipped. */
static int anim_run(void)
{
    int k;
    for (;;) {
        for (k = 0; k < ANIMS && !anim[k].kind; k++) ;
        if (k == ANIMS) return 0;
        wait_frame();
        if (key_hit(KEY_A | KEY_B)) { anim_finish(); return 1; }
    }
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
    anim_step();
    oam_copy(oam_mem, obuf, OBJ_USED);
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
    memset32((void *)gfx_fb, C_BG * 0x01010101u, GFX_W * GFX_H / 4);
}

/* Eight steps of brightness, written at the top of VBlank so a frame is all one level. Nothing is
   on screen at full black, so whatever the screen shows next can be loaded under it. */
static void fade(int out)
{
    int k;
    REG_BLDCNT = BLD_TOP_MASK | BLD_BLACK;
    for (k = 1; k <= 8; k++) {
        wait_frame();
        REG_BLDY = out ? 2 * k : 16 - 2 * k;
    }
    wait_frame();
    if (!out) REG_BLDCNT = BLD_TOP_MASK | BLD_WHITE;    /* A_FLASH's setting */
}

/* A full screen: its palette above the shared range, and its picture. Keep it under black or
   before the display is on: the old picture must not be shown in the new colours. Indexes 0-63
   are never touched, so a screen on the shared palette needs nothing put back afterwards. */
static void art_load(const u16 *pal, const u8 *pix)
{
    big.b = mini.b = NULL;
    memcpy16(pal_bg_mem + ART_PAL_FIRST, pal, ART_PAL_N);
    memcpy32((void *)gfx_fb, pix, GFX_W * GFX_H / 4);
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
    p->moat_next = 0;
    memset(p->shown, UNDRAWN, sizeof p->shown);
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
    int y = PANEL_Y + 8 * s, sunk = b->hits[s] >= ship_len[s], c = sunk ? C_RED : C_INK;
    gfx_rect(PANEL_X, y, 92, 7, C_BG);
    if (!ship_is_placed(b, s)) return;
    gfx_text(PANEL_X, y, ship_name[s], c);
    gfx_blit(ICON_X, y, ICON_W, ICON_H, art_icon[ship_len[s] - 1][sunk], ICON_W);
    if (sunk) gfx_rect(PANEL_X, y + 3, 6 * (int)strlen(ship_name[s]) - 1, 1, c);
}

/* ---------------------------------------------------------------- sound */
/* Bits 0-1: PSG 100%, which the music's levels are set for. Direct Sound A at 50% on both sides from timer 0: at
   100% a loud sample and the jingle together would pass the top of the mixer. Every write to REG_SNDDSCNT is this,
   with the FIFO reset bit or without. */
#define SND_MIX (SDS_DMG100 | SDS_A50 | SDS_AL | SDS_AR | SDS_ATMR0)
static void sound_init(void)
{
    REG_SNDSTAT = 0x80;         /* PSG on (must be first) */
    /* All channels both sides, master 7. Each PSG channel spans a quarter of the output, so all four at
       full volume just reach the top: the PSG can't clip against itself. */
    REG_SNDDMGCNT = 0xFF77;
    REG_SNDDSCNT = SND_MIX;
    REG_SND1SWEEP = 0;          /* never swept */
    /* CH3's wave, once only and with the channel off: writing wave RAM while CH3 plays is the classic
       bug. Bank 1 is selected, so the writes fill bank 0, which is the one a note plays. */
    REG_SND3SEL = 0x40;
    REG_WAVE_RAM0 = 0x67452301; REG_WAVE_RAM1 = 0xEFCDAB89;     /* a triangle: 0..F */
    REG_WAVE_RAM2 = 0x98BADCFE; REG_WAVE_RAM3 = 0x10325476;     /* and back down */
    REG_SND3SEL = 0;
}

/* Effects: one voice of samples on Direct Sound A, from ROM by DMA 1 at timer 0's rate. tools/gen_sfx.py makes
   them, each ending in enough silence that the output is at 0 when it stops and the DMA, which reads ahead of what
   has played, never leaves it. So no stop needs a fade, and nothing rings on: the longest sample ends before the
   quickest way back to the menu gets there (tools/e2e.py). Nothing here calls sim_rand. */
typedef struct { const s8 *data; u32 len; u16 frames; } Sfx;
extern const Sfx sfx_table[4];
extern const u16 sfx_reload;
enum { SFX_SPLASH, SFX_EXPLOSION, SFX_BOOM, SFX_PING };

/* From the VBlank handler at the end of a sample, and at each start. The DMA first, so it can't take one more
   word into a FIFO just reset. */
static void sfx_stop(void)
{
    REG_DMA1CNT = 0;
    REG_TM0CNT = 0;
    REG_SNDDSCNT = SND_MIX | SDS_ARESET;
}

/* A newer effect replaces the current one, but a sinking outlives a miss. The counter is cleared first and set
   last, so the handler can't stop this start part way, or with the last sample's count. */
static void sfx_play(int id)
{
    const Sfx *s = &sfx_table[id];
    if (id == SFX_SPLASH && sfx_cur == SFX_BOOM && sfx_left) return;
    sfx_left = 0;
    sfx_stop();
    REG_DMA1SAD = (u32)s->data;
    REG_DMA1DAD = (u32)&REG_FIFO_A;
    REG_DMA1CNT = DMA_ENABLE | DMA_AT_FIFO | DMA_REPEAT | DMA_32 | DMA_DST_FIXED;
    REG_TM0D = -sfx_reload;
    REG_TM0CNT = TM_ENABLE;
    sfx_cur = id;
    sfx_starts++;
    sfx_left = s->frames;
}

/* Music: CH1 melody, CH2 harmony, CH3 bass. A tune is three lists of notes, one entry per row, all stepped
   by one index, so they can't drift while they are the same length (tools/e2e.py checks that, and each
   tune's bars). A row is t->rate frames. _ lets the note ring on, REST silences the channel. */
enum {
    _, REST,
    C2, Db2, D2, Eb2, E2, F2, Gb2, G2, Ab2, A2, Bb2, B2,
    C3, Db3, D3, Eb3, E3, F3, Gb3, G3, Ab3, A3, Bb3, B3,
    C4, Db4, D4, Eb4, E4, F4, Gb4, G4, Ab4, A4, Bb4, B4,
    C5, Db5, D5, Eb5, E5, F5, Gb5, G5, Ab5, A5, Bb5, B5,
    C6, Db6, D6, Eb6, E6, F6, Gb6, G6, Ab6, A6, Bb6, B6,
    END = 0xFF
};
/* A square channel's frequency register for each note, from 131072 / Hz in the lowest octave. CH3 plays 32
   samples a cycle, half as fast for the same value, so it reads this table an octave up. */
#define OCTAVE(s) 2048 - (2004 >> s), 2048 - (1892 >> s), 2048 - (1785 >> s), 2048 - (1685 >> s), 2048 - (1591 >> s), 2048 - (1501 >> s), \
                  2048 - (1417 >> s), 2048 - (1337 >> s), 2048 - (1262 >> s), 2048 - (1192 >> s), 2048 - (1125 >> s), 2048 - (1062 >> s)
const u16 note_reg[] = { OCTAVE(0), OCTAVE(1), OCTAVE(2), OCTAVE(3), OCTAVE(4), OCTAVE(5) };

typedef struct { const u8 *voice[3]; u8 rate, loop; u16 cnt1, cnt2, cnt3; } Tune;

/* The title: 16 bars of 6/8 in G with a flat seventh, a bar a second. A horn-like fifth, rocking bass. */
static const u8 title_1[] = {
    G4, _, _, D5, _, _,    C5, _, D5, A4, _, _,   B4, _, D5, G5, _, _,    F5, _, E5, D5, _, _,
    G4, _, _, D5, _, _,    C5, _, D5, F5, _, _,   E5, _, D5, C5, _, A4,   B4, _, A4, G4, _, _,
    E5, _, G5, B5, _, _,   A5, _, G5, E5, _, _,   A4, _, C5, E5, _, _,    D5, _, F5, A5, _, _,
    G5, _, E5, C5, _, _,   F5, _, D5, C5, _, A4,  B4, _, A4, D5, _, _,    G4, _, _, D4, _, _, END };
static const u8 title_2[] = {
    D4, _, _, G4, _, _,    F4, _, _, F4, _, _,    G4, _, _, B4, _, _,     A4, _, _, F4, _, _,
    D4, _, _, G4, _, _,    F4, _, _, A4, _, _,    G4, _, _, E4, _, _,     G4, _, _, D4, _, _,
    G4, _, _, E5, _, _,    C5, _, _, A4, _, _,    E4, _, _, A4, _, _,     F4, _, _, D5, _, _,
    C5, _, _, G4, _, _,    A4, _, _, F4, _, _,    G4, _, _, B4, _, _,     D4, _, _, B3, _, _, END };
static const u8 title_3[] = {
    G2, _, _, D3, _, _,    F2, _, _, C3, _, _,    G2, _, _, D3, _, _,     D3, _, _, A2, _, _,
    G2, _, _, D3, _, _,    F2, _, _, C3, _, _,    C3, _, _, G2, _, _,     G2, _, D3, G2, _, _,
    E3, _, _, B2, _, _,    A2, _, _, E3, _, _,    A2, _, _, E3, _, _,     D3, _, _, A2, _, _,
    C3, _, _, G2, _, _,    F2, _, _, C3, _, _,    G2, _, _, D3, _, _,     G2, _, _, D3, _, _, END };
/* Battle: 12 bars of 8 slow rows, 32 s. A heartbeat of bass, and a few quiet notes between long rests. */
static const u8 battle_1[] = {
    _, _, _, _, _, _, _, _,    _, _, _, _, A4, _, _, _,    _, _, _, _, _, _, _, _,    _, _, _, _, Bb4, _, A4, _,
    _, _, _, _, _, _, _, _,    _, _, _, _, C5, _, Bb4, _,  A4, _, _, _, _, _, _, _,   _, _, _, _, _, _, _, _,
    _, _, _, _, F4, _, E4, _,  _, _, _, _, _, _, _, _,     _, _, _, _, E4, _, D4, _,  _, _, _, _, _, _, _, _, END };
static const u8 battle_2[] = {
    _, _, _, _, _, _, _, _,    _, _, _, _, _, _, _, _,     D4, _, _, _, _, _, _, _,   _, _, _, _, _, _, _, _,
    F4, _, _, _, _, _, _, _,   _, _, _, _, _, _, _, _,     E4, _, _, _, _, _, _, _,   _, _, _, _, _, _, _, _,
    Bb3, _, _, _, _, _, _, _,  _, _, _, _, _, _, _, _,     A3, _, _, _, _, _, _, _,   _, _, _, _, _, _, _, _, END };
static const u8 battle_3[] = {
    D3, _, REST, _, _, _, _, _,   D3, _, REST, _, A2, _, REST, _,   D3, _, REST, _, _, _, _, _,   D3, _, REST, _, A2, _, REST, _,
    D3, _, REST, _, _, _, _, _,   D3, _, REST, _, A2, _, REST, _,   D3, _, REST, _, _, _, _, _,   D3, _, REST, _, A2, _, REST, _,
    Bb2, _, REST, _, _, _, _, _,  Bb2, _, REST, _, F2, _, REST, _,  A2, _, REST, _, _, _, _, _,   A2, _, REST, _, E3, _, REST, _, END };
/* Win: up the call and over the top, 3.2 s. Lose: down and down, 4 s. Both inside the picture's 5 s. */
static const u8 win_1[] = {
    G4, _, B4, D5, _, G5,   F5, _, D5, G5, _, A5,   B5, _, _, G5, _, D5,   G5, _, _, _, _, _, END };
static const u8 win_2[] = {
    D4, _, G4, B4, _, D5,   C5, _, A4, D5, _, F5,   G5, _, _, D5, _, B4,   B4, _, _, _, _, _, END };
static const u8 win_3[] = {
    G2, _, _, D3, _, _,     F2, _, _, G2, _, _,     G2, _, _, D3, _, _,    G2, _, _, _, REST, _, END };
static const u8 lose_1[] = {
    E5, _, D5, C5, _, A4,   B4, _, G4, A4, _, F4,   E4, _, _, D4, _, C4,   A3, _, _, _, _, _, END };
static const u8 lose_2[] = {
    C5, _, B4, A4, _, F4,   G4, _, E4, F4, _, D4,   C4, _, _, B3, _, A3,   E3, _, _, _, _, _, END };
static const u8 lose_3[] = {
    A2, _, _, F2, _, _,     G2, _, _, D3, _, _,     A2, _, _, E2, _, _,    A2, _, _, _, REST, _, END };

/* The envelopes: volume nibble, then decay. CH3: 0x4000 is 50%, 0x6000 is 25%. Battle's are the quiet ones. */
const Tune tune_title  = { { title_1, title_2, title_3 },    10, 1, 0xA480, 0x6540, 0x4000 };
const Tune tune_battle = { { battle_1, battle_2, battle_3 }, 20, 1, 0x4780, 0x3780, 0x6000 };
const Tune tune_win    = { { win_1, win_2, win_3 },           8, 0, 0xC480, 0x8540, 0x4000 };
const Tune tune_lose   = { { lose_1, lose_2, lose_3 },       10, 0, 0xA480, 0x6540, 0x4000 };

const Tune *volatile music;     /* NULL: none. Set last and cleared first, so VBlank never sees half a change */
volatile u16 music_row, music_wait;
u16 music_last[3];              /* each channel's frequency value last written, 0 if silenced: those registers can't be read back */

/* Off, each by its own way, and the notes in hand with them. A sample playing is not touched. Retriggering at
   envelope volume 0 turns a square channel off. */
static void music_stop(void)
{
    music = NULL;
    REG_SND1CNT = 0; REG_SND1FREQ = 0x8000;
    REG_SND2CNT = 0; REG_SND2FREQ = 0x8000;
    REG_SND3SEL = 0;
    music_last[0] = music_last[1] = music_last[2] = 0;
}

static void music_play(const Tune *t)
{
    music_stop();
    music_row = 0;
    music_wait = 1;
    music = t;
}

/* From the VBlank handler, so nothing waits on it. Each note writes its channel's whole setup. */
static void music_step(void)
{
    const Tune *t = music;
    int n, r;
    if (!t || --music_wait) return;
    music_wait = t->rate;
    r = music_row;
    if (t->voice[0][r] == END) {
        if (!t->loop) { music_stop(); return; }
        r = 0;
    }
    music_row = r + 1;
    if ((n = t->voice[0][r]) != _) {
        REG_SND1CNT = n == REST ? 0 : t->cnt1;
        REG_SND1FREQ = 0x8000 | (music_last[0] = n == REST ? 0 : note_reg[n - C2]);
        if (n != REST) jingle_notes++;
    }
    if ((n = t->voice[1][r]) != _) {
        REG_SND2CNT = n == REST ? 0 : t->cnt2;
        REG_SND2FREQ = 0x8000 | (music_last[1] = n == REST ? 0 : note_reg[n - C2]);
        if (n != REST) jingle_notes++;
    }
    if ((n = t->voice[2][r]) == REST) { REG_SND3SEL = 0; music_last[2] = 0; }
    else if (n != _) {
        REG_SND3SEL = 0x80;
        REG_SND3CNT = t->cnt3;
        REG_SND3FREQ = 0x8000 | (music_last[2] = note_reg[n - C2 + 12]);
        jingle_notes++;
    }
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
    int f = 0, x = (GFX_W - 6 * 11) / 2;
    state = ST_TITLE;
    for (;;) {
        /* Only START leaves. Everything else is ignored, so mashing cannot start a game. */
        if (f % 120 == 0) {
            gfx_text(x + 1, START_Y + 1, "PRESS START", C_SHADOW);
            gfx_text(x, START_Y, "PRESS START", C_YEL);
        }
        if (f % 120 == 60)      /* the picture back from under the words */
            gfx_blit(x, START_Y, 66, 8, art_title + START_Y * GFX_W + x, GFX_W);
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

/* Returns 1 for manual placement. Key edges, so a held button cannot skip this screen. state says
   ST_MENU only once the keys are live, after the fade. */
static int menu(void)
{
    fade(1);
    music_stop();               /* the title tune, under black. Menu is silent */
    clear_screen();
    wait_frame();
    text_mid(40, "NAVAL HUNTER", C_INK);
    text_mid(70, "PLACE YOUR FLEET?", C_INK);
    text_mid(90, "A RANDOM   B MANUAL", C_INK);
    menu_level();
    fade(0);
    state = ST_MENU;
    for (;;) {
        wait_frame();
        if (key_hit(KEY_A)) return 0;
        if (key_hit(KEY_B)) return 1;
        if (key_hit(KEY_SELECT)) {
            ai_level = (ai_level + 1) % 4;
            menu_level();
            sfx_play(SFX_PING);
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
            big.b = NULL;               /* the board holds still until the battle clears it */
            for (; place_s < NSHIP; place_s++) {
                /* Failing is very unlikely, after a tight manual layout. The list then shows
                   a ship short, and alive must agree or that ship could never be sunk. */
                if (!place_random_one(&pb, place_s)) pb.alive--;
                panel_line(&pb, place_s);
                wait_frame();           /* a ship a frame: seven in one overran it, up to 338,000 cycles */
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

/* Message and sound for one resolved shot. who is "" for yours, "YOUR " for theirs. */
static void shot_result(int r, int sunk, const char *who, const char *miss, const char *hit)
{
    char b[24];
    if (r == 0) { msg(miss); sfx_play(SFX_SPLASH); return; }
    if (r == 1) { msg(hit); sfx_play(SFX_EXPLOSION); }
    else {
        strcpy(b, who);
        strcat(b, ship_name[sunk]);
        msg(strcat(b, " SUNK"));
        sfx_play(SFX_BOOM);
    }
}

/* Show a shot the rules have already resolved: the shell flies with the board held back, then it lands
   and the board, message and sound change together, then the impact plays. A or B skips what is left. */
static void shot_show(Pane *p, int i, int r, int sunk, int from_x, int from_y, const char *who, const char *miss, const char *hit)
{
    int half = p->size / 2, x = p->x + i % 10 * p->size + half, y = p->y + i / 10 * p->size + half, small = p == &mini;
    int len = OBJ_SPLASH_N * IMPACT_RATE, skipped, k, n = 0;
    held_pane = p;
    anim_add(A_SHELL, 0, SHELL_FRAMES, x, y, 0);
    anim[0].x0 = from_x;        /* the list is empty between shots, so the shell is entry 0 */
    anim[0].y0 = from_y;
    skipped = anim_run();
    held_pane = NULL;
    smoke_scan();
    if (small) {
        obj_set_pos(&obuf[1], SMALL_X + 8 * (i % 10), SMALL_Y + 8 * (i / 10));
        obj_unhide(&obuf[1], 0);
    } else if (r == 2) panel_line(&ob, sunk);
    shot_result(r, sunk, who, miss, hit);
    if (skipped) return;
    if (r == 0) anim_add(A_SPLASH, 0, len, x, y, small);
    else {
        len = OBJ_BLAST_N * IMPACT_RATE;
        if (r == 1) {
            anim_add(A_BLAST, 0, len, x, y, small);
            anim_add(A_FLASH, 0, FLASH_FRAMES, 8, 0, 0);
        } else {
            for (k = 0; k < 100; k++)
                if (p->b->own[k] == sunk)
                    anim_add(A_BLAST, SINK_STAGGER * n++, len, p->x + k % 10 * p->size + half, p->y + k / 10 * p->size + half, small);
            len += SINK_STAGGER * (n - 1);
            anim_add(A_FLASH, 0, FLASH_FRAMES + 4, 12, 0, 0);
            anim_add(A_SHAKE, 0, SHAKE_FRAMES, 0, 0, 0);
        }
    }
    anim_add(A_WAIT, 0, len + AFTER_SHOT, 0, 0, 0);
    anim_run();
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

static void ai_chunk_end(void)
{
    u32 c = cycles() - ai_chunk;
    if (c > ai_cycles) ai_cycles = c;
}

/* ai_between: HARD's map takes more than a frame, so it takes a frame between ships. */
static void ai_yield(void)
{
    ai_chunk_end();
    wait_frame();
    ai_chunk = cycles();
}

static int enemy_think(void)
{
    u32 t0 = frames;
    int i;
    ai_chunk = cycles();
    i = ai_next(&pb, ai_level);
    ai_chunk_end();
    while (frames - t0 < THINK_FRAMES) wait_frame();
    think_frames = frames - t0;
    return i;
}

static void enemy_turn(void)
{
    int i, x, y, r;
    u8 sunk = 0;
    status("ENEMY TURN");
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
    shot_show(&mini, y * 10 + x, r, sunk, THEIR_GUN_X, THEIR_GUN_Y, "YOUR ", "ENEMY MISS", "ENEMY HIT!");
}

/* Both boards and the fleet list. A piece a frame, and then the boards paint in. */
static void battle_chrome(int big_view, int mini_view)
{
    int s;
    clear_screen();
    wait_frame();
    pane_grid(&big, big_view);
    wait_frame();
    pane_grid(&mini, mini_view);
    wait_frame();
    big_labels();
    wait_frame();
    for (s = 0; s < NSHIP; s++) {
        panel_line(&ob, s);
        wait_frame();
    }
    big.b = &ob;
    mini.b = &pb;
    gfx_text(SMALL_X, 3, "YOUR WATERS", C_DIM);
    gfx_text(PANEL_X, PANEL_Y - 8, "ENEMY FLEET", C_DIM);
}

/* One game's shooting. Returns 1 if you won. */
static int battle(void)
{
    state = ST_BATTLE;
    cx = cy = 0;
    held_btn = held_timer = 0;
    shots_fired = shots_hit = 0;
    last_enemy = 0xFF;
    battle_chrome(VIEW_HIDDEN, VIEW_SHIPS);
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
        obj_hide(&obuf[0]);     /* no cursor while shells fly */
        msg("");
        shot_show(&big, cy * 10 + cx, r, sunk, YOUR_GUN_X, YOUR_GUN_Y, "", "MISS", "HIT!");
        if (ob.alive == 0) return 1;

        enemy_turn();
        if (pb.alive == 0) return 0;

        /* The enemy's result stays up while you aim; your next shot clears it. */
        state = ST_BATTLE;
        status("YOUR TURN");
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

/* The win or lose picture and its jingle. START or A skips both, but only on a fresh press after
   SPLASH_LOCK: key_hit is an edge, so a button still held from the last shot does nothing. */
static void splash(int won)
{
    int f;
    music_stop();               /* a shot's sample rings on */
    obj_hide(&obuf[0]);
    obj_hide(&obuf[1]);
    fade(1);
    art_load(won ? art_win_pal : art_lose_pal, won ? art_win : art_lose);
    smoke_scan();
    fade(0);
    state = ST_SPLASH;
    music_play(won ? &tune_win : &tune_lose);
    for (f = 0; f < SPLASH_FRAMES; f++) {
        wait_frame();
        if (f >= SPLASH_LOCK && key_hit(KEY_START | KEY_A)) break;
    }
    music_stop();               /* a skip: nothing carries on over the end screen */
}

/* Both fleets revealed. */
static void end_screen(int won)
{
    char b[24];
    int n;
    splash(won);
    fade(1);
    battle_chrome(VIEW_END, VIEW_END);
    status(won ? "YOU WIN!" : "YOU LOSE");
    n = fmt_u16(b, shots_fired);
    strcpy(b + n, " SHOTS ");
    n += 7;
    n += fmt_u16(b + n, shots_fired ? shots_hit * 100 / shots_fired : 0);
    strcpy(b + n, "%");
    msg(b);
    hint("START NEW GAME");
    pause_frames(6);            /* the boards paint in under black: 200 cells, under three frames */
    fade(0);
    state = ST_END;
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
    oam_init(obuf, OBJ_USED);
    /* the small board's impacts: 16 pixels of art to 12 */
    ((OBJ_AFFINE *)obuf)[1].pa = ((OBJ_AFFINE *)obuf)[1].pd = 341;
    ((OBJ_AFFINE *)obuf)[2].pa = ((OBJ_AFFINE *)obuf)[2].pd = 256;
    memcpy16(pal_bg_mem, art_shared_pal, ART_PAL_FIRST);
    memcpy16(pal_obj_mem, art_obj_pal, 16);
    memcpy16(&tile_mem[5][OBJ_ART - 512], art_obj, OBJ_TILES * 16);
    /* Mode 4 keeps sprite tiles from index 512: lower ones are hidden by the bitmap. */
    make_box(&tile_mem[5][0], 2, 13, 1);
    make_box(&tile_mem[5][4], 1, 8, 2);
    obj_set_attr(&obuf[0], ATTR0_SQUARE | ATTR0_HIDE, ATTR1_SIZE_16, 512);
    obj_set_attr(&obuf[1], ATTR0_SQUARE | ATTR0_HIDE, ATTR1_SIZE_8, 516);
    REG_TM2D = REG_TM3D = 0;
    REG_TM3CNT = TM_CASCADE | TM_ENABLE;
    REG_TM2CNT = TM_ENABLE;
    REG_BLDCNT = BLD_TOP_MASK | BLD_WHITE;      /* every layer and the backdrop towards white */
    ai_between = ai_yield;
    sound_init();
    gfx_fb = (volatile u16 *)MEM_VRAM;
    art_load(art_title_pal, art_title);
    REG_DISPCNT = DCNT_MODE4 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
    irq_init(NULL);
    irq_add(II_VBLANK, on_vbl);

    music_play(&tune_title);
    title_screen();             /* outside the loop: a replay goes straight to the menu */
    for (;;) {
        int manual = menu();
        music_play(&tune_battle);       /* through placement too, and on until splash */
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
