#ifndef GFX_H_INCLUDED
#define GFX_H_INCLUDED
#include <stdint.h>

#define GFX_W 240
#define GFX_H 160

extern volatile uint16_t *gfx_fb;   /* 240x160 at 8bpp; main.c points it at the page */

/* Neither clips: keep them on screen. gfx_text draws ink only, 6px a character. */
void gfx_rect(int x, int y, int w, int h, uint8_t c);
void gfx_text(int x, int y, const char *s, uint8_t c);
/* Copies w x h from src, whose rows are stride apart. Any x, odd or even. */
void gfx_blit(int x, int y, int w, int h, const uint8_t *src, int stride);
/* Which hull segment (art.h's SEG_*) a ship's cell is, from its neighbours in own[] with the same
   ship id. gfx_seg_of says the same for cell k of a ship not on the board yet. */
int gfx_segment(const uint8_t *own, int i);
int gfx_seg_of(int k, int len, int horiz);

#endif
