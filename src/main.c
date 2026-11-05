/* Naval Hunter for the GBA. Phase 1: boots into Mode 4 and holds a test screen. */
#include <tonc.h>
#include "gfx.h"

enum { ST_BOOT, ST_READY };
enum { C_SEA, C_INK, C_RED, C_YEL };

/* Read by tools/e2e.py through the .sym file, so not static. */
u8 state = ST_BOOT;
volatile u32 frames, frame_drops;

static volatile u8 work_done = 1;

static void on_vbl(void)
{
    frames++;
    if (!work_done) frame_drops++;  /* this VBlank came before the frame's work ended */
    work_done = 0;
}

/* The one place the game waits. */
static void wait_frame(void)
{
    work_done = 1;
    VBlankIntrWait();
}

static void test_screen(void)
{
    gfx_rect(0, 0, GFX_W, GFX_H, C_SEA);
    gfx_text(84, 40, "NAVAL HUNTER", C_INK);
    gfx_text(9, 60, "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ", C_INK);
    gfx_text(9, 70, "!',-./:<>=[]", C_INK);
    /* tools/e2e.py checks these. Each pair shares a halfword, in both drawing orders. */
    gfx_rect(100, 90, 1, 1, C_RED); gfx_rect(101, 90, 1, 1, C_YEL);
    gfx_rect(101, 92, 1, 1, C_YEL); gfx_rect(100, 92, 1, 1, C_RED);
    gfx_rect(105, 94, 4, 2, C_RED);  /* odd start, odd end */
}

int main(void)
{
    REG_WAITCNT = WS_STANDARD;
    oam_init(oam_mem, 128);
    pal_bg_mem[C_SEA] = RGB15(2, 6, 14);
    pal_bg_mem[C_INK] = RGB15(31, 31, 31);
    pal_bg_mem[C_RED] = RGB15(31, 4, 4);
    pal_bg_mem[C_YEL] = RGB15(31, 31, 4);
    gfx_fb = (volatile u16 *)MEM_VRAM;
    test_screen();
    REG_DISPCNT = DCNT_MODE4 | DCNT_BG2 | DCNT_OBJ | DCNT_OBJ_1D;
    irq_init(NULL);
    irq_add(II_VBLANK, on_vbl);
    state = ST_READY;
    for (;;) wait_frame();
}
