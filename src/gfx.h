#ifndef GFX_H_INCLUDED
#define GFX_H_INCLUDED
#include <stdint.h>

#define GFX_W 240
#define GFX_H 160

extern volatile uint16_t *gfx_fb;   /* 240x160 at 8bpp; main.c points it at the page */

/* Neither clips: keep them on screen. gfx_text draws ink only, 6px a character. */
void gfx_rect(int x, int y, int w, int h, uint8_t c);
void gfx_text(int x, int y, const char *s, uint8_t c);

#endif
