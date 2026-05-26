/*
 * NeOS tile launcher GUI.
 *
 * Layout (160x120 pixels, 5x7 font):
 *
 *   y= 0..11   title bar      "NeOS 0.4   tile launcher"
 *   y=14..107  tile area      3 columns x 2 rows, tile = 50x44, gap=2
 *   y=110..119 status bar     "  WASD / arrows = move    ENTER  ESC"
 *
 * Each tile shows: filled bg, 1-px border, centered label.
 * Selected tile gets a brighter border + accent bg + white label.
 *
 * Apps launched (mapped to existing cmd_* functions in main.c).
 */

#include <stdint.h>
#include "fb.h"
#include "font5x7.h"
#include "gui.h"

/* From main.c — UART helpers + dispatcher targets. */
extern int  uart_getc_nb(void);
extern void puts_both(const char *s);
extern void cmd_mandel(void);
extern void cmd_pong(void);
extern void cmd_synth(void);
extern void cmd_imgtest(void);
extern void cmd_img(void);
extern void cmd_cam(void);
extern void cmd_matrix(void);
extern void cmd_info(void);
extern void cmd_ascii(void);
extern void edit_run(void);

/* TERMINAL tile: hand HDMI back to the text terminal until user hits a key. */
static void launch_terminal(void) {
    extern void fb_show(int);
    fb_show(0);
    puts_both("\r\n-- TERMINAL view (HDMI shows green text). Press any key. --\r\n");
    while (uart_getc_nb() < 0) { }
}

/* ---- palette: dark navy desktop + warm accent ---- */
static const uint32_t GUI_PAL[16] = {
    0x00081020u, /* 0  bg  deep navy   */
    0x00ffffffu, /* 1  white            */
    0x00b0b0b0u, /* 2  light gray       */
    0x00606060u, /* 3  mid gray         */
    0x00202840u, /* 4  tile bg          */
    0x00303860u, /* 5  tile hi-light    */
    0x00ffcc33u, /* 6  accent yellow    */
    0x00ff6633u, /* 7  accent orange    */
    0x00ff3366u, /* 8  accent pink      */
    0x00cc33ffu, /* 9  accent purple    */
    0x003366ffu, /* 10 accent blue      */
    0x0033ccffu, /* 11 cyan             */
    0x0033ffccu, /* 12 mint             */
    0x0066ff33u, /* 13 lime             */
    0x00000000u, /* 14 black            */
    0x00ffff99u, /* 15 cream            */
};

#define C_BG       0
#define C_WHITE    1
#define C_GRAY     2
#define C_DARKGRAY 3
#define C_TILE     4
#define C_TILE_HI  5
#define C_YELLOW   6
#define C_ORANGE   7
#define C_PINK     8
#define C_PURPLE   9
#define C_BLUE     10
#define C_CYAN     11
#define C_MINT     12
#define C_LIME     13
#define C_BLACK    14
#define C_CREAM    15

typedef struct {
    const char *label;
    uint8_t     accent;
    void      (*launch)(void);
} tile_t;

static const tile_t TILES[] = {
    { "MANDEL", C_YELLOW, cmd_mandel  },
    { "PONG",   C_LIME,   cmd_pong    },
    { "SYNTH",  C_CYAN,   cmd_synth   },
    { "IMG",    C_ORANGE, cmd_img     },
    { "CAM",    C_PINK,   cmd_cam     },
    { "MATRIX", C_MINT,   cmd_matrix  },
    { "ASCII",  C_BLUE,   cmd_ascii   },
    { "EDIT",   C_CREAM,  edit_run    },
    { "TERM",   C_PURPLE, launch_terminal },
};
#define N_TILES ((int)(sizeof(TILES) / sizeof(TILES[0])))
#define N_COLS  3
#define N_ROWS  3

#define TITLE_H  12
#define STATUS_Y 110
#define TILE_W   50
#define TILE_H   30
#define TILE_GAP 2
#define GRID_X   ((FB_W - (N_COLS * TILE_W + (N_COLS - 1) * TILE_GAP)) / 2)
#define GRID_Y   14

static int gui_selected = 0;

/* Center a label horizontally within a tile of width TILE_W. */
static int label_x(const char *s, int tile_x) {
    int n = 0;
    while (s[n]) ++n;
    int pw = n * (FONT_W + 1) - 1;
    return tile_x + (TILE_W - pw) / 2;
}

static void render_row(int y, fb_row *row) {
    /* 1. base background */
    fb_row_fill(row, C_BG);

    /* 2. title bar */
    if (y < TITLE_H) {
        fb_row_fill(row, C_BLACK);
        if (y >= 2 && y <= 8) {
            int cr = y - 2;       /* 0..6 across FONT_H=7 */
            fb_row_text(row, 4, cr,  "NeOS 0.4",  C_WHITE, 0, 0);
            fb_row_text(row, 86, cr, "LAUNCHER", C_YELLOW, 0, 0);
        }
        /* underline */
        if (y == TITLE_H - 1) fb_row_hseg(row, 0, FB_W - 1, C_GRAY);
        return;
    }

    /* 3. status bar */
    if (y >= STATUS_Y) {
        fb_row_fill(row, C_BLACK);
        if (y >= STATUS_Y + 1 && y <= STATUS_Y + 7) {
            int cr = y - (STATUS_Y + 1);
            fb_row_text(row, 4,   cr, "WASD",   C_CYAN,   0, 0);
            fb_row_text(row, 32,  cr, "MOVE",   C_GRAY,   0, 0);
            fb_row_text(row, 64,  cr, "ENTER",  C_LIME,   0, 0);
            fb_row_text(row, 102, cr, "GO",     C_GRAY,   0, 0);
            fb_row_text(row, 122, cr, "ESC",    C_PINK,   0, 0);
            fb_row_text(row, 144, cr, "X",      C_GRAY,   0, 0);
        }
        /* top divider */
        if (y == STATUS_Y) fb_row_hseg(row, 0, FB_W - 1, C_GRAY);
        return;
    }

    /* 4. tile grid */
    for (int idx = 0; idx < N_TILES; ++idx) {
        int col = idx % N_COLS;
        int rowi = idx / N_COLS;
        int tx = GRID_X + col * (TILE_W + TILE_GAP);
        int ty = GRID_Y + rowi * (TILE_H + TILE_GAP);
        if (y < ty || y >= ty + TILE_H) continue;

        int selected = (idx == gui_selected);
        uint8_t bg     = selected ? C_TILE_HI : C_TILE;
        uint8_t border = selected ? TILES[idx].accent : C_DARKGRAY;
        uint8_t txt    = selected ? C_WHITE : C_GRAY;
        uint8_t accent = TILES[idx].accent;

        /* fill the tile body for this scanline */
        fb_row_hseg(row, tx, tx + TILE_W - 1, bg);

        /* border */
        int local = y - ty;
        if (local == 0 || local == TILE_H - 1) {
            fb_row_hseg(row, tx, tx + TILE_W - 1, border);
            /* outer accent bar on selected */
            if (selected && local == 0)
                fb_row_hseg(row, tx, tx + TILE_W - 1, accent);
        } else {
            fb_row_pixel(row, tx, border);
            fb_row_pixel(row, tx + TILE_W - 1, border);
        }

        /* accent stripe at top of every tile (icon-ish band) */
        if (local >= 2 && local <= 9) {
            fb_row_hseg(row, tx + 2, tx + TILE_W - 3, accent);
        }

        /* label centered vertically around row ~30 of the tile */
        int label_top = TILE_H / 2 + 4;
        int label_cr  = local - label_top;
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

/* ---- input ----
 * The host UART sends raw characters. We read them through uart_getc_nb
 * so we can be non-blocking (so the GUI could later animate). For now we
 * just spin and read what comes in.
 *
 * Arrow keys come as ESC '[' 'A'|'B'|'C'|'D'. We track a tiny state
 * machine for the prefix. WASD/HJKL also recognized.
 */
typedef enum { S_IDLE, S_ESC, S_BRK } esc_state_t;

static int get_action(void) {
    static esc_state_t st = S_IDLE;
    int c;
    for (;;) {
        c = uart_getc_nb();
        if (c < 0) continue;
        switch (st) {
            case S_IDLE:
                if (c == 0x1B) { st = S_ESC; continue; }
                if (c == '\r' || c == '\n') return 'E';   /* enter */
                if (c == 'w' || c == 'W' || c == 'k' || c == 'K') return 'U';
                if (c == 's' || c == 'S' || c == 'j' || c == 'J') return 'D';
                if (c == 'a' || c == 'A' || c == 'h' || c == 'H') return 'L';
                if (c == 'd' || c == 'D' || c == 'l' || c == 'L') return 'R';
                if (c == 'q' || c == 'Q') return 'X';
                /* unknown printable: ignore */
                break;
            case S_ESC:
                if (c == '[') { st = S_BRK; continue; }
                st = S_IDLE;
                return 'X';       /* lone ESC = exit */
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

void gui_run(void) {
    puts_both("\r\nGUI launcher (WASD/arrows + ENTER, ESC=exit)\r\n");

    fb_set_palette(GUI_PAL);
    fb_show(1);
    render_all();

    for (;;) {
        int act = get_action();
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
                    /* Returning from the app: rebuild the GUI. */
                    fb_set_palette(GUI_PAL);
                    fb_show(1);
                    render_all();
                }
                break;
            }
            case 'X':
                fb_show(0);
                puts_both("\r\nGUI off\r\n");
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
