/*
 * NeOS Desktop — Ubuntu-inspired tile launcher.
 *
 * Layout (160x120, 5x7 font):
 *
 *   y= 0..107  desktop: aubergine wallpaper (gradient + dot pattern)
 *              with 2x2 tile grid centered
 *   y=108..119 taskbar: charcoal with orange top edge,
 *              "NeOS" left, "up M:SS" right
 *
 * Tiles: TERM, EDIT, INFO, ABOUT. Each = 50x40 with 6px gap, 2x2 grid.
 */

#include <stdint.h>
#include "fb.h"
#include "font5x7.h"
#include "gui.h"
#include "sched.h"

extern int  uart_getc_nb(void);
extern void puts_both(const char *s);
extern void cmd_info(void);
extern void edit_run(void);

/* Forward decl: implemented in this file. */
static void cmd_about(void);

/* TERMINAL tile: drop into NeOS shell REPL. */
extern void repl_run(void);
static void launch_terminal(void) {
    extern void fb_show(int);
    fb_show(0);
    repl_run();
}

/* Ubuntu-inspired palette: aubergine background, orange accent. */
static const uint32_t GUI_PAL[16] = {
    0x00170010u, /* 0  bg deepest aubergine    */
    0x002C001Eu, /* 1  bg aubergine            */
    0x004A0E2Eu, /* 2  bg aubergine light      */
    0x00772A4Cu, /* 3  bg aubergine highlight  */
    0x00E95420u, /* 4  Ubuntu orange           */
    0x00DD4814u, /* 5  orange dark             */
    0x00FFB186u, /* 6  orange light            */
    0x002A2A2Au, /* 7  taskbar charcoal        */
    0x00505050u, /* 8  border / inactive       */
    0x00AAAAAAu, /* 9  text secondary          */
    0x00FFFFFFu, /* 10 text primary            */
    0x0000B89Fu, /* 11 teal icon               */
    0x006BB7FFu, /* 12 sky icon                */
    0x00E1BC4Eu, /* 13 gold icon               */
    0x0050C878u, /* 14 emerald icon            */
    0x00000000u, /* 15 black                   */
};

#define C_BG0     0
#define C_BG1     1
#define C_BG2     2
#define C_BG3     3
#define C_ACCENT  4
#define C_ACC_D   5
#define C_ACC_L   6
#define C_BAR     7
#define C_BORDER  8
#define C_TXT2    9
#define C_TXT1    10
#define C_ICON1   11
#define C_ICON2   12
#define C_ICON3   13
#define C_ICON4   14
#define C_BLACK   15

typedef struct {
    const char *label;
    uint8_t     accent;
    void      (*launch)(void);
} tile_t;

static const tile_t TILES[] = {
    { "TERM",  C_ICON2, launch_terminal },
    { "EDIT",  C_ICON3, edit_run        },
    { "INFO",  C_ICON1, cmd_info        },
    { "ABOUT", C_ACCENT, cmd_about      },
};
#define N_TILES ((int)(sizeof(TILES) / sizeof(TILES[0])))
#define N_COLS  2
#define N_ROWS  2

#define BAR_Y    108
#define BAR_H    12

#define TILE_W   50
#define TILE_H   40
#define TILE_GAP 6
#define GRID_W   (N_COLS * TILE_W + (N_COLS - 1) * TILE_GAP)
#define GRID_H   (N_ROWS * TILE_H + (N_ROWS - 1) * TILE_GAP)
#define GRID_X   ((FB_W - GRID_W) / 2)
#define GRID_Y   ((BAR_Y - GRID_H) / 2)

static int gui_selected = 0;

/* Center a label horizontally within a tile. */
static int label_x(const char *s, int tile_x) {
    int n = 0;
    while (s[n]) ++n;
    int pw = n * (FONT_W + 1) - 1;
    return tile_x + (TILE_W - pw) / 2;
}

/* Wallpaper: smooth 4-band gradient + faint diagonal dot mesh. */
static uint8_t wallpaper_color(int x, int y) {
    uint8_t base;
    if      (y < BAR_Y / 4)      base = C_BG0;
    else if (y < BAR_Y / 2)      base = C_BG1;
    else if (y < 3 * BAR_Y / 4)  base = C_BG2;
    else                          base = C_BG2;

    /* Subtle diagonal stripes every 24 px for texture. */
    int diag = (x + y) % 24;
    if (diag == 0) return C_BG3;
    if (diag == 12 && (y & 1) == 0) return C_BG2;
    return base;
}

/* Render uptime string "up M:SS" into a fixed buffer. */
static void format_uptime(char *buf) {
    uint32_t s = irq_ticks / 100;
    uint32_t m = s / 60;
    s %= 60;
    /* "up M:SS\0" — supports up to 999 minutes. */
    char *p = buf;
    *p++ = 'u'; *p++ = 'p'; *p++ = ' ';
    if (m >= 100) *p++ = '0' + (m / 100);
    if (m >= 10)  *p++ = '0' + ((m / 10) % 10);
    *p++ = '0' + (m % 10);
    *p++ = ':';
    *p++ = '0' + (s / 10);
    *p++ = '0' + (s % 10);
    *p   = 0;
}

static void render_row(int y, fb_row *row) {
    /* --- TASKBAR --- */
    if (y >= BAR_Y) {
        if (y == BAR_Y) {
            /* top edge: 1px orange line as accent strip */
            fb_row_fill(row, C_ACCENT);
            return;
        }
        fb_row_fill(row, C_BAR);
        if (y >= BAR_Y + 3 && y <= BAR_Y + 9) {
            int cr = y - (BAR_Y + 3);
            /* "NeOS" left, white */
            fb_row_text(row, 4, cr, "NeOS", C_TXT1, 0, 0);
            /* uptime right, aligned to width */
            static char ubuf[10];
            if (cr == 0) format_uptime(ubuf);
            int n = 0; while (ubuf[n]) n++;
            int pw = n * (FONT_W + 1) - 1;
            fb_row_text(row, FB_W - pw - 4, cr, ubuf, C_TXT2, 0, 0);
        }
        return;
    }

    /* --- DESKTOP --- */
    /* wallpaper base */
    for (int x = 0; x < FB_W; ++x) {
        fb_row_pixel(row, x, wallpaper_color(x, y));
    }

    /* --- TILES (2x2 grid) --- */
    for (int idx = 0; idx < N_TILES; ++idx) {
        int col = idx % N_COLS;
        int rowi = idx / N_COLS;
        int tx = GRID_X + col * (TILE_W + TILE_GAP);
        int ty = GRID_Y + rowi * (TILE_H + TILE_GAP);

        /* drop shadow: drawn 2 rows below tile bottom */
        if (y == ty + TILE_H || y == ty + TILE_H + 1) {
            fb_row_hseg(row, tx + 2, tx + TILE_W + 1, C_BG0);
        }

        if (y < ty || y >= ty + TILE_H) continue;

        int selected = (idx == gui_selected);
        uint8_t bg     = selected ? C_BG3 : C_BG1;
        uint8_t border = selected ? TILES[idx].accent : C_BORDER;
        uint8_t border2 = selected ? C_ACC_L : C_BORDER;
        uint8_t txt    = selected ? C_TXT1 : C_TXT2;
        uint8_t accent = TILES[idx].accent;
        int local = y - ty;

        /* tile body */
        fb_row_hseg(row, tx, tx + TILE_W - 1, bg);

        /* drop shadow on right edge (1 col right) */
        fb_row_pixel(row, tx + TILE_W,     C_BG0);
        fb_row_pixel(row, tx + TILE_W + 1, C_BG0);

        /* outer border: 2px when selected, 1px otherwise */
        if (local == 0 || local == TILE_H - 1) {
            fb_row_hseg(row, tx, tx + TILE_W - 1, border);
            if (selected && (local == 0 || local == TILE_H - 1)) {
                /* top edge thicker */
            }
        } else if (selected && (local == 1 || local == TILE_H - 2)) {
            /* inner glow row */
            fb_row_hseg(row, tx + 1, tx + TILE_W - 2, border2);
        } else {
            fb_row_pixel(row, tx, border);
            fb_row_pixel(row, tx + TILE_W - 1, border);
            if (selected) {
                fb_row_pixel(row, tx + 1, border2);
                fb_row_pixel(row, tx + TILE_W - 2, border2);
            }
        }

        /* icon area: accent band 8px tall, centered horizontally with margin */
        if (local >= 4 && local <= 18) {
            /* accent rectangle */
            fb_row_hseg(row, tx + 8, tx + TILE_W - 9, accent);
            /* inset highlight on top edge of accent */
            if (local == 4) {
                fb_row_hseg(row, tx + 8, tx + TILE_W - 9, C_ACC_L);
            }
            /* small dark notch in center for "icon" feel */
            if (local >= 9 && local <= 13) {
                int cx = tx + TILE_W / 2;
                fb_row_pixel(row, cx - 1, C_BG0);
                fb_row_pixel(row, cx,     C_BG0);
                fb_row_pixel(row, cx + 1, C_BG0);
            }
        }

        /* label centered near bottom */
        int label_top = TILE_H - FONT_H - 5;
        int label_cr = local - label_top;
        if (label_cr >= 0 && label_cr < FONT_H) {
            fb_row_text(row, label_x(TILES[idx].label, tx),
                        label_cr, TILES[idx].label, txt, bg, 0);
        }
    }
}

static void render_all(void) {
    fb_row row;
    for (int y = 0; y < FB_H; ++y) {
        render_row(y, &row);
        fb_row_commit(y, &row);
    }
}

/* Repaint just the taskbar (cheap, ~12 rows) so uptime ticks live.
 * Forward-declared above so get_action_blocking can call it. */
static void render_taskbar(void) {
    fb_row row;
    for (int y = BAR_Y; y < FB_H; ++y) {
        render_row(y, &row);
        fb_row_commit(y, &row);
    }
}

/*
 * Input: blocking read. Idle ticks taskbar so uptime stays live.
 * Arrow keys (ESC '[' A/B/C/D) processed atomically here so no chars
 * get dropped between main loop iterations.
 */
typedef enum { S_IDLE, S_ESC, S_BRK } esc_state_t;

static void render_taskbar(void);  /* forward */

static int get_action_blocking(void) {
    static esc_state_t st = S_IDLE;
    static uint32_t last_tick = 0;
    for (;;) {
        int c = uart_getc_nb();
        if (c < 0) {
            uint32_t now = irq_ticks / 100;
            if (now != last_tick) {
                last_tick = now;
                render_taskbar();
            }
            continue;
        }
        switch (st) {
            case S_IDLE:
                if (c == 0x1B) { st = S_ESC; continue; }
                if (c == '\r' || c == '\n') return 'E';
                if (c == 'w' || c == 'W' || c == 'k') return 'U';
                if (c == 's' || c == 'S' || c == 'j') return 'D';
                if (c == 'a' || c == 'A' || c == 'h') return 'L';
                if (c == 'd' || c == 'D' || c == 'l') return 'R';
                if (c == 'q' || c == 'Q') return 'X';
                break;
            case S_ESC:
                if (c == '[') { st = S_BRK; continue; }
                /* Lone ESC = ignore (avoid picocom DTR-break false-exit). */
                st = S_IDLE;
                break;
            case S_BRK:
                st = S_IDLE;
                if (c == 'A') return 'U';
                if (c == 'B') return 'D';
                if (c == 'D') return 'L';
                if (c == 'C') return 'R';
                break;
        }
    }
}

/* ABOUT screen — simple full-screen credits. */
static void cmd_about(void) {
    puts_both("\r\n");
    puts_both("  +--------------------------------+\r\n");
    puts_both("  |          NeOS  v0.4            |\r\n");
    puts_both("  +--------------------------------+\r\n");
    puts_both("  | picorv32 RV32IMC @ 27 MHz      |\r\n");
    puts_both("  | Tang Nano 9K (GW1NR-LV9)       |\r\n");
    puts_both("  | 32 KB BRAM, 8.6K LUT           |\r\n");
    puts_both("  | HDMI 800x600  / 160x120 fb     |\r\n");
    puts_both("  | preemptive scheduler (Stage 1) |\r\n");
    puts_both("  | mini-C interp + cc compiler    |\r\n");
    puts_both("  +--------------------------------+\r\n");
    puts_both("\r\n  Press any key to return.\r\n");
    while (uart_getc_nb() < 0) { }
}

void gui_run(void) {
    puts_both("\r\nNeOS Desktop (WASD/arrows + ENTER, ESC=exit)\r\n");

    fb_set_palette(GUI_PAL);
    fb_show(1);
    render_all();

    for (;;) {
        int act = get_action_blocking();
        int col = gui_selected % N_COLS;
        int row = gui_selected / N_COLS;
        switch (act) {
            case 'U': if (row > 0)         row -= 1; break;
            case 'D': if (row < N_ROWS - 1) row += 1; break;
            case 'L': if (col > 0)         col -= 1; break;
            case 'R': if (col < N_COLS - 1) col += 1; break;
            case 'E': {
                int idx = row * N_COLS + col;
                if (idx < N_TILES) {
                    fb_show(0);
                    puts_both("\r\n[launching ");
                    puts_both(TILES[idx].label);
                    puts_both("]\r\n");
                    TILES[idx].launch();
                    fb_set_palette(GUI_PAL);
                    fb_show(1);
                    render_all();
                }
                break;
            }
            case 'X':
                fb_show(0);
                puts_both("\r\nDesktop off\r\n");
                return;
        }
        int new_sel = row * N_COLS + col;
        if (new_sel >= N_TILES) new_sel = N_TILES - 1;
        if (new_sel != gui_selected) {
            gui_selected = new_sel;
            render_all();
        }
    }
}
