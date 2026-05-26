/*
 * NeOS vim-subset editor.
 *
 * Buffer is a single contiguous char array. Newlines separate lines.
 * Cursor is a single byte offset into the buffer; (line, col) derived on
 * demand. Visible region (`scroll`..`scroll+VIS_ROWS-1`) is rendered each
 * time something changes.
 *
 * Drawing: full FB redraw per change (~50 ms). No partial repaint.
 */

#include <stdint.h>
#include "fb.h"
#include "font5x7.h"
#include "edit.h"
#include "cc.h"

extern int  uart_getc_nb(void);
extern void puts_both(const char *s);
extern void put_dec(uint32_t);

/* ---- buffer ---- */
#define EDIT_BUF_SZ 384
static char ebuf[EDIT_BUF_SZ];
static int  elen   = 0;
static int  ecur   = 0;        /* byte offset of cursor */
static int  escroll = 0;       /* first visible line */

/* ---- modes ---- */
enum { M_NORMAL = 0, M_INSERT = 1, M_CMD = 2 };
static int emode = M_NORMAL;

/* operator-pending: 'd' waits for another 'd' for dd */
static int e_op_d = 0;

/* command line */
static char ecmd[32];
static int  ecmd_len = 0;
static int  e_quit = 0;

/* layout */
#define VIS_COLS 26
#define VIS_ROWS 13
#define CH_W     6              /* font 5 + 1 gap */
#define CH_H     8              /* font 7 + 1 gap */
#define STATUS_Y 0
#define TEXT_Y   (STATUS_Y + CH_H)
#define CMD_Y    (TEXT_Y + VIS_ROWS * CH_H)

/* ---- palette: aubergine bg, vivid cursor + line highlight ---- */
static const uint32_t EDIT_PAL[16] = {
    0x001A1A2Eu, /* 0  bg dark            */
    0x00F0F0F0u, /* 1  text fg            */
    0x00808890u, /* 2  dim                */
    0x00E95420u, /* 3  cursor block (Ubuntu orange) */
    0x0024243Cu, /* 4  status bar bg      */
    0x00FFCC33u, /* 5  mode tag (NORMAL)  */
    0x0050C878u, /* 6  mode tag (INSERT)  */
    0x006BB7FFu, /* 7  mode tag (COMMAND) */
    0x00FFB186u, /* 8  filename / accent  */
    0x00606888u, /* 9  line numbers       */
    0x0050C878u, /* 10 ok msg             */
    0x00FF3366u, /* 11 error msg          */
    0x002A2A4Au, /* 12 current line bg    */
    0x00FFFFFFu, /* 13 cursor text fg     */
    0, 0,
};

#define C_BG       0
#define C_FG       1
#define C_DIM      2
#define C_CUR      3
#define C_STATBG   4
#define C_MNORM    5
#define C_MINS     6
#define C_MCMD     7
#define C_CURLINE  12
#define C_CURFG    13
#define C_ACCENT   8
#define C_LN       9
#define C_OK       10
#define C_ERR      11

static const char *EDIT_INIT = "print(7*13);";

/* ---- helpers ---- */
static int e_strlen(const char *s) { int n = 0; while (s[n]) ++n; return n; }

static void e_load_initial(void) {
    int n = e_strlen(EDIT_INIT);
    for (int i = 0; i < n && i < EDIT_BUF_SZ; ++i) ebuf[i] = EDIT_INIT[i];
    elen = n;
    ecur = 0;
    escroll = 0;
}

static int e_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void e_pos_to_lc(int pos, int *line, int *col) {
    int l = 0, c = 0;
    for (int i = 0; i < pos && i < elen; ++i) {
        if (ebuf[i] == '\n') { ++l; c = 0; } else { ++c; }
    }
    *line = l; *col = c;
}

static int e_line_start(int line) {
    int l = 0;
    for (int i = 0; i < elen; ++i) {
        if (l == line) return i;
        if (ebuf[i] == '\n') ++l;
    }
    return elen;
}

static int e_line_len(int line) {
    int s = e_line_start(line);
    int e = s;
    while (e < elen && ebuf[e] != '\n') ++e;
    return e - s;
}

static int e_line_count(void) {
    int n = 1;
    for (int i = 0; i < elen; ++i) if (ebuf[i] == '\n') ++n;
    return n;
}

/* Move cursor to (line, col), clamping col to line length. */
static void e_goto(int line, int col) {
    int nlines = e_line_count();
    line = e_clamp(line, 0, nlines - 1);
    int len = e_line_len(line);
    col = e_clamp(col, 0, len);
    ecur = e_line_start(line) + col;
}

/* Insert character at cursor. Cursor advances. */
static void e_insert(char c) {
    if (elen >= EDIT_BUF_SZ - 1) return;
    for (int i = elen; i > ecur; --i) ebuf[i] = ebuf[i - 1];
    ebuf[ecur] = c;
    ++elen;
    ++ecur;
}

/* Delete char to the LEFT of cursor (backspace in insert). */
static void e_backspace(void) {
    if (ecur == 0) return;
    for (int i = ecur - 1; i < elen - 1; ++i) ebuf[i] = ebuf[i + 1];
    --elen;
    --ecur;
}

/* Delete char AT cursor (vim 'x'). */
static void e_delete_at(void) {
    if (ecur >= elen) return;
    if (ebuf[ecur] == '\n') return;   /* don't merge lines via x */
    for (int i = ecur; i < elen - 1; ++i) ebuf[i] = ebuf[i + 1];
    --elen;
}

/* Delete current line (vim 'dd'). */
static void e_delete_line(void) {
    int line, col;
    e_pos_to_lc(ecur, &line, &col);
    int s = e_line_start(line);
    int e = s + e_line_len(line);
    if (e < elen && ebuf[e] == '\n') ++e;   /* include trailing newline */
    int n = e - s;
    for (int i = s; i < elen - n; ++i) ebuf[i] = ebuf[i + n];
    elen -= n;
    if (elen < 0) elen = 0;
    if (ecur > elen) ecur = elen;
    /* place cursor at start of line (now next line) */
    int nlines = e_line_count();
    if (line >= nlines) line = nlines - 1;
    e_goto(line, 0);
}

/* ---- scrolling: keep cursor visible ---- */
static void e_fix_scroll(void) {
    int line, col;
    e_pos_to_lc(ecur, &line, &col);
    if (line < escroll) escroll = line;
    if (line >= escroll + VIS_ROWS) escroll = line - VIS_ROWS + 1;
    if (escroll < 0) escroll = 0;
}

/* ---- rendering ---- */
/* Draw glyph `ch` at (xpos, cr) into `row`. fg is the glyph color,
 * `bg_color` (>=0) optionally paints the full 5-wide cell background. */
static void draw_glyph(fb_row *row, int xpos, int cr, char ch,
                       uint8_t fg, int bg_color) {
    if (bg_color >= 0) {
        for (int b = 0; b < CH_W; ++b)
            fb_row_pixel(row, xpos + b, (uint8_t)bg_color);
    }
    if (cr >= FONT_H) return;
    const uint8_t *g = font5x7[(unsigned char)ch];
    uint8_t bits = g[cr];
    for (int b = 0; b < FONT_W; ++b)
        if ((bits >> (FONT_W - 1 - b)) & 1)
            fb_row_pixel(row, xpos + b, fg);
}

/* render the whole screen — every scanline written from scratch,
 * no fb_fill, no inter-line gaps left stale */
static void render_all(void) {
    fb_row row;
    int line_cursor, col_cursor;
    e_pos_to_lc(ecur, &line_cursor, &col_cursor);

    /* ---- status (8 rows) ---- */
    {
        const char *mode_str =
            (emode == M_NORMAL) ? "NORMAL" :
            (emode == M_INSERT) ? "INSERT" :
                                  "CMD   ";
        uint8_t mode_col =
            (emode == M_NORMAL) ? C_MNORM :
            (emode == M_INSERT) ? C_MINS  :
                                  C_MCMD;

        char pos[24];
        int pi = 0;
        pos[pi++] = 'L';
        if (line_cursor + 1 >= 100) pos[pi++] = '0' + ((line_cursor + 1) / 100) % 10;
        if (line_cursor + 1 >= 10)  pos[pi++] = '0' + ((line_cursor + 1) / 10)  % 10;
        pos[pi++] = '0' + (line_cursor + 1) % 10;
        pos[pi++] = ' ';
        pos[pi++] = 'C';
        if (col_cursor + 1 >= 100) pos[pi++] = '0' + ((col_cursor + 1) / 100) % 10;
        if (col_cursor + 1 >= 10)  pos[pi++] = '0' + ((col_cursor + 1) / 10)  % 10;
        pos[pi++] = '0' + (col_cursor + 1) % 10;
        pos[pi]   = '\0';

        for (int cr = 0; cr < CH_H; ++cr) {
            fb_row_fill(&row, C_STATBG);
            if (cr < FONT_H) {
                int x = 2;
                for (const char *p = mode_str; *p; ++p) {
                    draw_glyph(&row, x, cr, *p, mode_col, -1);
                    x += CH_W;
                }
                x += 4;
                for (const char *p = "scratch.cc"; *p; ++p) {
                    draw_glyph(&row, x, cr, *p, C_ACCENT, -1);
                    x += CH_W;
                }
                int pl = e_strlen(pos);
                int px = FB_W - 2 - pl * CH_W;
                for (const char *p = pos; *p; ++p) {
                    draw_glyph(&row, px, cr, *p, C_DIM, -1);
                    px += CH_W;
                }
            }
            fb_row_commit(STATUS_Y + cr, &row);
        }
    }

    /* ---- text area: 13 lines × 8 rows = 104 ---- */
    for (int vrow = 0; vrow < VIS_ROWS; ++vrow) {
        int line_idx = escroll + vrow;
        int line_y   = TEXT_Y + vrow * CH_H;
        int valid    = (line_idx < e_line_count());

        int s = valid ? e_line_start(line_idx) : 0;
        int len = valid ? e_line_len(line_idx) : 0;

        char lnbuf[3];
        int  ln = line_idx + 1;
        lnbuf[0] = valid ? ((ln >= 100) ? '0' + (ln / 100) % 10 : ' ') : ' ';
        lnbuf[1] = valid ? ((ln >= 10)  ? '0' + (ln / 10)  % 10 : ' ') : ' ';
        lnbuf[2] = valid ? '0' + ln % 10 : '~';

        int text_start_x = 3 * CH_W + 2;
        int max_chars    = (FB_W - text_start_x) / CH_W;
        if (len > max_chars) len = max_chars;

        int is_cur_line = (line_idx == line_cursor);
        for (int cr = 0; cr < CH_H; ++cr) {
            fb_row_fill(&row, is_cur_line ? C_CURLINE : C_BG);
            if (cr < FONT_H) {
                /* line number gutter */
                int x = 0;
                for (int i = 0; i < 3; ++i) {
                    draw_glyph(&row, x, cr, lnbuf[i],
                               is_cur_line ? C_FG : C_LN, -1);
                    x += CH_W;
                }
                /* gutter | separator */
                if (cr < FONT_H) {
                    fb_row_pixel(&row, 3 * CH_W, C_DIM);
                }
                /* text + cursor highlighting */
                int cur_col = is_cur_line ? col_cursor : -1;
                if (cur_col > max_chars) cur_col = max_chars;

                for (int i = 0; i < len; ++i) {
                    int is_cur = (i == cur_col);
                    draw_glyph(&row, text_start_x + i * CH_W, cr,
                               ebuf[s + i],
                               is_cur ? C_CURFG : C_FG,
                               is_cur ? (int)C_CUR : -1);
                }
                /* cursor past end of line: draw empty highlighted cell */
                if (is_cur_line && cur_col >= len && cur_col < max_chars) {
                    draw_glyph(&row, text_start_x + cur_col * CH_W, cr,
                               ' ', C_CURFG, (int)C_CUR);
                }
            }
            fb_row_commit(line_y + cr, &row);
        }
    }

    /* ---- command row (8 rows) ---- */
    for (int cr = 0; cr < CH_H; ++cr) {
        fb_row_fill(&row, C_STATBG);
        if (cr < FONT_H) {
            if (emode == M_CMD) {
                int x = 2;
                draw_glyph(&row, x, cr, ':', C_MCMD, -1); x += CH_W;
                for (int i = 0; i < ecmd_len; ++i) {
                    draw_glyph(&row, x, cr, ecmd[i], C_FG, -1);
                    x += CH_W;
                }
                /* cmd cursor underscore */
                if (cr == FONT_H - 1)
                    for (int b = 0; b < CH_W; ++b)
                        fb_row_pixel(&row, x + b, C_CUR);
            } else {
                const char *hint = (emode == M_INSERT)
                    ? "INSERT  -- ESC to leave"
                    : "i a o O  x dd  :w :run :q";
                int x = 2;
                for (const char *p = hint; *p; ++p) {
                    draw_glyph(&row, x, cr, *p, C_DIM, -1);
                    x += CH_W;
                }
            }
        }
        fb_row_commit(CMD_Y + cr, &row);
    }
}

/* ---- modal output box ---- */
extern int  capture_mode;
extern char capture_buf[128];
extern int  capture_len;

/* Draw a centered modal box showing up to MAX_BOX_LINES lines of captured
 * cc output. Each line wraps at MAX_BOX_COLS chars (LF starts a new line). */
#define BOX_X       16
#define BOX_Y       28
#define BOX_W       (FB_W - 2 * BOX_X)   /* 128 */
#define BOX_H       72
#define MAX_BOX_COLS ((BOX_W - 8) / CH_W)
#define MAX_BOX_LINES ((BOX_H - 18) / CH_H)

static void modal_show(void) {
    fb_row row;
    /* Build wrapped lines from capture_buf into a small array */
    char lines[16][22];      /* up to 16 lines x 21 chars + NUL */
    int  nlines = 0;
    int  col = 0;
    for (int i = 0; i < capture_len && nlines < 16; ++i) {
        char c = capture_buf[i];
        if (c == '\r') continue;
        if (c == '\n' || col >= 21) {
            lines[nlines][col] = '\0';
            ++nlines;
            col = 0;
            if (c == '\n') continue;
        }
        if (nlines < 16) lines[nlines][col++] = c;
    }
    if (nlines < 16 && col > 0) { lines[nlines][col] = '\0'; ++nlines; }

    /* Render: full-screen redraw, with box overlay. */
    for (int y = 0; y < FB_H; ++y) {
        int in_box = (y >= BOX_Y && y < BOX_Y + BOX_H);
        if (!in_box) continue;        /* leave the editor pixels alone */

        fb_row_fill(&row, C_STATBG);  /* box body */
        int local_y = y - BOX_Y;

        /* border */
        if (local_y == 0 || local_y == BOX_H - 1) {
            for (int x = BOX_X; x < BOX_X + BOX_W; ++x)
                fb_row_pixel(&row, x, C_ACCENT);
        } else {
            for (int x = 0; x < BOX_X; ++x) fb_row_pixel(&row, x, C_BG);
            for (int x = BOX_X + BOX_W; x < FB_W; ++x) fb_row_pixel(&row, x, C_BG);
            fb_row_pixel(&row, BOX_X, C_ACCENT);
            fb_row_pixel(&row, BOX_X + BOX_W - 1, C_ACCENT);
        }
        /* clear outside the box back to editor bg */
        for (int x = 0; x < BOX_X; ++x) fb_row_pixel(&row, x, C_BG);
        for (int x = BOX_X + BOX_W; x < FB_W; ++x) fb_row_pixel(&row, x, C_BG);

        /* title row inside box */
        if (local_y >= 2 && local_y < 2 + FONT_H) {
            int cr = local_y - 2;
            int x = BOX_X + 4;
            for (const char *p = "RUN OUTPUT"; *p; ++p) {
                draw_glyph(&row, x, cr, *p, C_MCMD, -1);
                x += CH_W;
            }
        }
        /* text body, starting at local_y = 12 */
        int body_start = 12;
        int line_idx = (local_y - body_start) / CH_H;
        int cr       = (local_y - body_start) % CH_H;
        if (local_y >= body_start && line_idx < nlines && cr < FONT_H) {
            int x = BOX_X + 4;
            for (const char *p = lines[line_idx]; *p; ++p) {
                draw_glyph(&row, x, cr, *p, C_FG, -1);
                x += CH_W;
            }
        }
        /* footer hint at bottom */
        if (local_y >= BOX_H - 9 && local_y < BOX_H - 9 + FONT_H) {
            int cr2 = local_y - (BOX_H - 9);
            int x = BOX_X + 4;
            for (const char *p = "[any key]"; *p; ++p) {
                draw_glyph(&row, x, cr2, *p, C_DIM, -1);
                x += CH_W;
            }
        }
        fb_row_commit(y, &row);
    }
}

/* ---- command handling ---- */
static void e_do_command(void) {
    /* very small parser */
    if (ecmd_len == 0) return;
    if (ecmd_len == 1 && ecmd[0] == 'q') {
        e_quit = 1;
        return;
    }
    if (ecmd_len == 5 && ecmd[0] == 'c' && ecmd[1] == 'l' && ecmd[2] == 'e' &&
        ecmd[3] == 'a' && ecmd[4] == 'r') {
        elen = 0; ecur = 0; escroll = 0;
        return;
    }
    if (ecmd_len == 1 && ecmd[0] == 'w') {
        /* compile (don't run) — print result to UART */
        extern uint32_t *cc_code_base_dummy(void);
        /* compile into the existing cc buffer */
        ebuf[elen] = '\0';
        int n = cc_compile(ebuf, (uint32_t *)0x00007100u, 960);
        if (n >= 0) {
            puts_both("\r\n[edit:w] compiled "); put_dec((uint32_t)n);
            puts_both(" instr\r\n");
        } else {
            puts_both("\r\n[edit:w] compile error\r\n");
        }
        return;
    }
    if (ecmd_len == 3 && ecmd[0] == 'r' && ecmd[1] == 'u' && ecmd[2] == 'n') {
        ebuf[elen] = '\0';
        int n = cc_compile(ebuf, (uint32_t *)0x00007100u, 960);
        if (n < 0) { puts_both("\r\n[edit:run] compile error\r\n"); return; }
        puts_both("\r\n[edit:run] compiled "); put_dec((uint32_t)n);
        puts_both(" instr; running\r\n");

        /* Capture cc output to a buffer; FB stays on so we can pop modal. */
        capture_len  = 0;
        capture_mode = 1;
        cc_run((uint32_t *)0x00007100u);
        capture_mode = 0;

        modal_show();
        /* wait for any key, then redraw editor */
        while (uart_getc_nb() < 0) { }
        return;
    }
    if ((ecmd_len == 2 && ecmd[0] == 's' && ecmd[1] == 'p') ||
        (ecmd_len == 3 && ecmd[0] == 'v' && ecmd[1] == 's' && ecmd[2] == 'p')) {
        puts_both("\r\n[edit] sp/vsp: screen too small\r\n");
        return;
    }
    puts_both("\r\n[edit] unknown :cmd\r\n");
}

/* ---- input loop ---- */
enum { K_ESC = 0x1B };

static int wait_key(void) {
    int c;
    do { c = uart_getc_nb(); } while (c < 0);
    return c;
}

/* Handle ESC-bracket arrows -> single keys L/R/U/D */
static int read_key_translated(void) {
    int c = wait_key();
    if (c != K_ESC) return c;
    /* may be lone ESC or arrow sequence */
    int c2 = uart_getc_nb();
    if (c2 != '[') return K_ESC;
    int c3 = wait_key();
    switch (c3) {
        case 'A': return 'k';
        case 'B': return 'j';
        case 'C': return 'l';
        case 'D': return 'h';
        default:  return c;
    }
}

void edit_run(void) {
    e_load_initial();
    emode = M_NORMAL;
    fb_set_palette(EDIT_PAL);
    fb_show(1);
    render_all();
    puts_both("\r\nedit: vim-subset launched. ESC quits to NORMAL, :q to exit.\r\n");

    while (!e_quit) {
        int c = read_key_translated();

        if (emode == M_CMD) {
            if (c == '\r' || c == '\n') {
                e_do_command();
                emode = M_NORMAL;
                ecmd_len = 0;
            } else if (c == K_ESC) {
                emode = M_NORMAL;
                ecmd_len = 0;
            } else if (c == 0x7F || c == 0x08) {
                if (ecmd_len > 0) --ecmd_len;
                else { emode = M_NORMAL; }
            } else if (c >= 0x20 && c < 0x7F && ecmd_len < (int)sizeof(ecmd) - 1) {
                ecmd[ecmd_len++] = (char)c;
            }
            render_all();
            continue;
        }

        if (emode == M_INSERT) {
            if (c == K_ESC) {
                emode = M_NORMAL;
            } else if (c == '\r' || c == '\n') {
                e_insert('\n');
            } else if (c == 0x7F || c == 0x08) {
                e_backspace();
            } else if (c >= 0x20 && c < 0x7F) {
                e_insert((char)c);
            }
            e_fix_scroll();
            render_all();
            continue;
        }

        /* NORMAL mode */
        if (e_op_d) {
            if (c == 'd') { e_delete_line(); }
            e_op_d = 0;
            e_fix_scroll();
            render_all();
            continue;
        }

        int line, col;
        e_pos_to_lc(ecur, &line, &col);

        switch (c) {
            case 'h': e_goto(line, col - 1); break;
            case 'l': e_goto(line, col + 1); break;
            case 'k': e_goto(line - 1, col); break;
            case 'j': e_goto(line + 1, col); break;
            case '0': e_goto(line, 0); break;
            case '$': e_goto(line, e_line_len(line)); break;
            case 'g': e_goto(0, 0); break;
            case 'G': e_goto(e_line_count() - 1, 0); break;
            case 'i': emode = M_INSERT; break;
            case 'a': {
                if (col < e_line_len(line)) ++ecur;
                emode = M_INSERT;
                break;
            }
            case 'o': {
                e_goto(line, e_line_len(line));
                e_insert('\n');
                emode = M_INSERT;
                break;
            }
            case 'O': {
                e_goto(line, 0);
                e_insert('\n');
                e_goto(line, 0);
                emode = M_INSERT;
                break;
            }
            case 'x': e_delete_at(); break;
            case 'd': e_op_d = 1; break;
            case ':': emode = M_CMD; ecmd_len = 0; break;
            case K_ESC: /* nothing in NORMAL */ break;
            default: break;
        }
        e_fix_scroll();
        render_all();
    }

    e_quit = 0;
    fb_show(0);
    puts_both("\r\n[edit] bye\r\n");
}
