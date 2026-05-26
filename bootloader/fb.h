/*
 * NeOS framebuffer drawing primitives.
 *
 * Backing store is the on-die FB BRAM mapped at:
 *   palette: 0x20000000 (16 × 32-bit, low 24 bits = 0x00_RR_GG_BB)
 *   pixels:  0x20004000 (2400 words, 8 px/word, 4 bpp, nibble 0 = leftmost)
 *
 * The hardware framebuffer is write-only from the CPU's side, so drawing
 * functions operate on a single scratch ROW buffer (`fb_row`) of 20 words
 * (160 pixels). Higher-level code composes a row, then calls fb_row_commit
 * to write the whole row to FB BRAM in one pass.
 *
 * Enable bit (mem-mapped at 0x10000040 bit 0) controls whether the HDMI
 * pipeline shows the framebuffer (1) or the text terminal (0).
 */

#ifndef NEOS_FB_H
#define NEOS_FB_H

#include <stdint.h>

#define FB_W            160
#define FB_H            120
#define FB_WORDS_PER_ROW 20

#define FB_PALETTE   ((volatile uint32_t *)0x20000000u)
#define FB_PIXELS    ((volatile uint32_t *)0x20004000u)
#define FB_ENABLE    (*(volatile uint32_t *)0x10000040u)

/* Set on/off (raw enable register). */
void fb_show(int on);

/* Load 16-entry palette (each entry 0x00_RR_GG_BB). */
void fb_set_palette(const uint32_t pal[16]);

/* Fill the entire FB with a solid color index. */
void fb_fill(uint8_t color);

/* Scratch row buffer used by the row-based drawing helpers below. */
typedef struct {
    uint32_t w[FB_WORDS_PER_ROW];
} fb_row;

void fb_row_fill(fb_row *r, uint8_t color);
void fb_row_pixel(fb_row *r, int x, uint8_t color);
void fb_row_hseg(fb_row *r, int x0, int x1, uint8_t color);     /* inclusive */
void fb_row_text(fb_row *r, int x, int char_row, const char *s,
                 uint8_t fg, uint8_t bg, int draw_bg);
void fb_row_commit(int y, const fb_row *r);

/* Composite helpers — these compose into row buffers internally and commit. */
void fb_rect_fill(int x, int y, int w, int h, uint8_t color);
void fb_rect_border(int x, int y, int w, int h, uint8_t color);
void fb_text(int x, int y, const char *s, uint8_t fg);
void fb_text_bg(int x, int y, const char *s, uint8_t fg, uint8_t bg);

#endif /* NEOS_FB_H */
