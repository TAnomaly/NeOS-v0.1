#include "fb.h"
#include "font5x7.h"

void fb_show(int on) {
    FB_ENABLE = on ? 1u : 0u;
}

void fb_set_palette(const uint32_t pal[16]) {
    for (int i = 0; i < 16; ++i) FB_PALETTE[i] = pal[i];
}

/* Build a 32-bit word made entirely of one 4-bit color, repeated 8x. */
static uint32_t pack8(uint8_t c) {
    uint32_t n = (uint32_t)(c & 0xF);
    uint32_t w = 0;
    w |= n <<  0;
    w |= n <<  4;
    w |= n <<  8;
    w |= n << 12;
    w |= n << 16;
    w |= n << 20;
    w |= n << 24;
    w |= n << 28;
    return w;
}

void fb_fill(uint8_t color) {
    uint32_t w = pack8(color);
    for (int i = 0; i < FB_W * FB_H / 8; ++i) FB_PIXELS[i] = w;
}

void fb_row_fill(fb_row *r, uint8_t color) {
    uint32_t w = pack8(color);
    for (int i = 0; i < FB_WORDS_PER_ROW; ++i) r->w[i] = w;
}

void fb_row_pixel(fb_row *r, int x, uint8_t color) {
    if (x < 0 || x >= FB_W) return;
    int word_idx  = x >> 3;
    int nib_pos   = x & 7;
    uint32_t mask = 0xFu << (nib_pos * 4);
    uint32_t bits = ((uint32_t)(color & 0xF)) << (nib_pos * 4);
    r->w[word_idx] = (r->w[word_idx] & ~mask) | bits;
}

void fb_row_hseg(fb_row *r, int x0, int x1, uint8_t color) {
    if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= FB_W) x1 = FB_W - 1;
    for (int x = x0; x <= x1; ++x) fb_row_pixel(r, x, color);
}

void fb_row_text(fb_row *r, int x, int char_row, const char *s,
                 uint8_t fg, uint8_t bg, int draw_bg) {
    if (char_row < 0 || char_row >= FONT_H) return;
    int cx = x;
    while (*s) {
        const uint8_t *g = font5x7[(unsigned char)*s];
        uint8_t bits = g[char_row];   /* low 5 bits = pixels, MSB-first */
        for (int b = 0; b < FONT_W; ++b) {
            int on = (bits >> (FONT_W - 1 - b)) & 1;
            if (on)            fb_row_pixel(r, cx + b, fg);
            else if (draw_bg)  fb_row_pixel(r, cx + b, bg);
        }
        if (draw_bg) fb_row_pixel(r, cx + FONT_W, bg);   /* 1-px gap */
        cx += FONT_W + 1;
        ++s;
    }
}

void fb_row_commit(int y, const fb_row *r) {
    if (y < 0 || y >= FB_H) return;
    int base = y * FB_WORDS_PER_ROW;
    for (int i = 0; i < FB_WORDS_PER_ROW; ++i) FB_PIXELS[base + i] = r->w[i];
}

/* ----- whole-rect helpers ----- */
void fb_rect_fill(int x, int y, int w, int h, uint8_t color) {
    fb_row row;
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= FB_H) continue;
        /* read-modify by composing: we don't have FB readback, so we
         * fill a fresh row with bg = palette index 0 then paint the
         * segment we care about. Use a scratch row pre-filled to 0 for
         * untouched pixels. Callers expecting a true "rectangle only"
         * effect should clear the rest first via fb_fill(). */
        fb_row_fill(&row, 0);
        fb_row_hseg(&row, x, x + w - 1, color);
        fb_row_commit(yy, &row);
    }
}

void fb_rect_border(int x, int y, int w, int h, uint8_t color) {
    fb_row row;
    for (int yy = y; yy < y + h; ++yy) {
        if (yy < 0 || yy >= FB_H) continue;
        fb_row_fill(&row, 0);
        if (yy == y || yy == y + h - 1) {
            fb_row_hseg(&row, x, x + w - 1, color);
        } else {
            fb_row_pixel(&row, x, color);
            fb_row_pixel(&row, x + w - 1, color);
        }
        fb_row_commit(yy, &row);
    }
}

void fb_text(int x, int y, const char *s, uint8_t fg) {
    fb_row row;
    for (int cr = 0; cr < FONT_H; ++cr) {
        fb_row_fill(&row, 0);
        fb_row_text(&row, x, cr, s, fg, 0, 0);
        fb_row_commit(y + cr, &row);
    }
}

void fb_text_bg(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    fb_row row;
    for (int cr = 0; cr < FONT_H; ++cr) {
        fb_row_fill(&row, bg);
        fb_row_text(&row, x, cr, s, fg, bg, 1);
        fb_row_commit(y + cr, &row);
    }
}
