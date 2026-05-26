#include <stdint.h>

#define UART_TX_DATA  (*(volatile uint32_t *)0x10000000)
#define UART_STATUS   (*(volatile uint32_t *)0x10000004)
#define UART_RX_DATA  (*(volatile uint32_t *)0x10000008)
#define LED_REG       (*(volatile uint32_t *)0x10000010)
#define TERM_DATA     (*(volatile uint32_t *)0x10000020)
#define TERM_STATUS   (*(volatile uint32_t *)0x10000024)

#include "interp.h"
#include "cc.h"
#include "gui.h"
#include "sched.h"
extern void cc_setup_syscalls(void);
#define CC_CODE_BASE  ((uint32_t *)0x00006100u)
#define CC_CODE_MAX   960

#define ST_TX_BUSY    (1u << 0)
#define ST_RX_VALID   (1u << 1)
#define ST_TERM_BUSY  (1u << 0)

#define APP_BASE      0x00006000u
#define APP_MAX_SIZE  (32u * 1024u - APP_BASE)

/* --- low-level UART --- */
void uart_putc_raw(char c) {
    while (UART_STATUS & ST_TX_BUSY) ;
    UART_TX_DATA = (uint32_t)(uint8_t)c;
}

void term_putc_raw(char c) {
    while (TERM_STATUS & ST_TERM_BUSY) ;
    TERM_DATA = (uint32_t)(uint8_t)c;
}

/* Mirror to both UART and HDMI terminal. */
void putc_both(char c) {
    uart_putc_raw(c);
    term_putc_raw(c);
}

void puts_both(const char *s) {
    while (*s) putc_both(*s++);
}

static uint8_t uart_getc_blocking(void) {
    while (!(UART_STATUS & ST_RX_VALID)) ;
    return (uint8_t)(UART_RX_DATA & 0xFF);
}

int uart_getc_nb(void) {
    if (!(UART_STATUS & ST_RX_VALID)) return -1;
    return (int)(UART_RX_DATA & 0xFF);
}

static uint32_t read_u32_le(void) {
    uint32_t v = 0;
    v |= (uint32_t)uart_getc_blocking() <<  0;
    v |= (uint32_t)uart_getc_blocking() <<  8;
    v |= (uint32_t)uart_getc_blocking() << 16;
    v |= (uint32_t)uart_getc_blocking() << 24;
    return v;
}

/* --- formatted output --- */
static const char HEX[16] = "0123456789ABCDEF";

void put_hex8(uint32_t v) {
    for (int i = 7; i >= 0; --i) putc_both(HEX[(v >> (i * 4)) & 0xF]);
}

static void put_hex2(uint8_t v) {
    putc_both(HEX[(v >> 4) & 0xF]);
    putc_both(HEX[ v       & 0xF]);
}

void put_dec(uint32_t v) {
    char buf[11];
    int  n = 0;
    if (!v) { putc_both('0'); return; }
    while (v) { buf[n++] = '0' + (v % 10); v /= 10; }
    while (n--) putc_both(buf[n]);
}

/* --- input helpers --- */
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_space(char c) { return c == ' ' || c == '\t'; }

static const char *skip_spaces(const char *s) {
    while (*s && is_space(*s)) ++s;
    return s;
}

/* parse hex (with or without 0x), advance *pp past it. returns 1 if parsed. */
static int parse_hex(const char **pp, uint32_t *out) {
    const char *p = skip_spaces(*pp);
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t v = 0;
    int n = 0;
    int d;
    while ((d = hex_digit(*p)) >= 0) { v = (v << 4) | (uint32_t)d; ++p; ++n; }
    if (!n) return 0;
    *out = v;
    *pp  = p;
    return 1;
}

/* parse decimal, advance *pp. returns 1 if parsed. */
static int parse_dec(const char **pp, uint32_t *out) {
    const char *p = skip_spaces(*pp);
    uint32_t v = 0;
    int n = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p - '0'); ++p; ++n; }
    if (!n) return 0;
    *out = v;
    *pp  = p;
    return 1;
}

/* compare tok against word (case-insensitive), tok ends at space/null. */
static int tok_eq(const char *tok, const char *word) {
    while (*word) {
        char t = *tok++;
        if (t >= 'A' && t <= 'Z') t += 32;
        if (t != *word++) return 0;
    }
    return *tok == '\0' || is_space(*tok);
}

/* --- line editor: read a line into buf, echo to both --- */
static int read_line(char *buf, int max) {
    int n = 0;
    for (;;) {
        uint8_t c = uart_getc_blocking();
        if (c == '\r' || c == '\n') {
            putc_both('\r'); putc_both('\n');
            buf[n] = '\0';
            return n;
        }
        if (c == 0x7F || c == 0x08) {                  // backspace
            if (n > 0) {
                --n;
                putc_both('\b'); putc_both(' '); putc_both('\b');
            }
            continue;
        }
        if (c < 0x20) continue;                         // ignore other ctrl
        if (n < max - 1) {
            buf[n++] = (char)c;
            putc_both((char)c);                         // echo
        }
    }
}

/* --- commands --- */
static void cmd_help(void) {
    puts_both("legacy commands:\r\n");
    puts_both("  help                 this list\r\n");
    puts_both("  clear                clear screens\r\n");
    puts_both("  mandel               ASCII Mandelbrot fractal\r\n");
    puts_both("  matrix               'rain' animation, any key to stop\r\n");
    puts_both("  pong                 AI-vs-AI Pong demo\r\n");
    puts_both("  guess                guess 1..100\r\n");
    puts_both("  hangman              word-guess game\r\n");
    puts_both("  ascii                ASCII printable table\r\n");
    puts_both("  info                 system info\r\n");
    puts_both("  led <hex>            write LED (low 6 bits)\r\n");
    puts_both("  peek <addr>          read 32-bit word\r\n");
    puts_both("  poke <addr> <val>    write 32-bit word\r\n");
    puts_both("  dump <addr> <len>    hex dump N bytes\r\n");
    puts_both("  mul <a> <b>          decimal multiply\r\n");
    puts_both("  hex <dec>            print as hex\r\n");
    puts_both("  u                    upload binary to 0x2000\r\n");
    puts_both("  g | run              jump to 0x2000\r\n");
    puts_both("mini-C interpreter:\r\n");
    puts_both("  let x = EXPR         declare/assign var (a-z, A-Z)\r\n");
    puts_both("  x = EXPR             assign var\r\n");
    puts_both("  print EXPR           print signed decimal\r\n");
    puts_both("  printh EXPR          print as 0xHHHHHHHH\r\n");
    puts_both("  EXPR                 evaluate, print result\r\n");
    puts_both("  ops: + - * / % & | ^ ~ << >> < <= > >= == != && || !\r\n");
    puts_both("  builtins: led(v) peek(a) poke(a,v) delay(ms) read() write(b) tone(hz)\r\n");
    puts_both("  examples:\r\n");
    puts_both("    let x = 7*13\r\n");
    puts_both("    print x*x\r\n");
    puts_both("    led(0x2A)\r\n");
    puts_both("    poke(0x10000010, 0)\r\n");
    puts_both("mini-C compiler (compiles to RV32IM, runs native):\r\n");
    puts_both("  cc <C source>      compile + jump\r\n");
    puts_both("  example:\r\n");
    puts_both("    cc int x=0; while (x<6) { led(1<<x); delay(150); x=x+1; }\r\n");
    puts_both("    cc print(7*13);\r\n");
    puts_both("    cc puts(\"Hello from native code!\");\r\n");
}

/* --------- PRNG (LFSR-32) --------- */
static uint32_t prng_state = 0xDEADBEEF;
static uint32_t prng_next(void) {
    /* Xorshift32 */
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x;
    return x;
}

/* Mix some entropy from UART bit noise / cycle count proxy */
static void prng_stir(uint32_t e) { prng_state ^= e * 0x9E3779B1u; }

/* --------- info --------- */
void cmd_info(void) {
    puts_both("NeOS v0.3  /  picorv32 (RV32IMC)  /  27 MHz\r\n");
    puts_both("32 KB BRAM total\r\n");
    puts_both("  bootloader region: 0x0000-0x5FFF (24 KB)\r\n");
    puts_both("  cc vars:           0x6000-0x607F\r\n");
    puts_both("  cc syscall tbl:    0x6080-0x60FF\r\n");
    puts_both("  cc code buffer:    0x6100-0x6FFF\r\n");
    puts_both("  app upload slot:   0x6000-0x7FFF\r\n");
    puts_both("Peripherals: UART115200, 6 LEDs, HDMI 640x480 text\r\n");
}

/* --------- ascii table --------- */
void cmd_ascii(void) {
    uint32_t c;
    for (c = 32; c < 127; ++c) {
        put_hex2((uint8_t)c);
        putc_both(' ');
        putc_both((char)c);
        putc_both(' ');
        if ((c & 7) == 7) puts_both("\r\n");
    }
    puts_both("\r\n");
}

/* --------- ASCII art splash for boot --------- */

/* --------- number guess game --------- */
static void cmd_guess(void) {
    prng_stir(0x12345);
    uint32_t target = (prng_next() % 100) + 1;
    int tries = 0;
    char line[16];
    puts_both("Number 1..100. Guess.\r\n");
    for (;;) {
        ++tries;
        puts_both("guess> ");
        read_line(line, sizeof line);
        const char *p = line;
        uint32_t v;
        if (!parse_dec(&p, &v)) { puts_both("bad number\r\n"); continue; }
        if (v == target) {
            puts_both("YES! in ");
            put_dec((uint32_t)tries);
            puts_both(" tries\r\n");
            return;
        }
        puts_both(v < target ? "higher\r\n" : "lower\r\n");
    }
}

/* --------- hangman --------- */
static const char *HANG_WORDS[] = {
    "PICORV", "TANGNANO", "VERILOG", "RISCV", "FPGA",
    "MANDELBROT", "CIRCUIT", "PROCESSOR", "BOOTLOADER", "COMPILER"
};
#define HANG_N 10

static int hang_contains(const char *s, char c) {
    while (*s) { if (*s == c) return 1; ++s; }
    return 0;
}

static int hang_strlen(const char *s) { int n=0; while (*s) {++n; ++s;} return n; }

static void cmd_hangman(void) {
    prng_stir(0xABC);
    const char *word = HANG_WORDS[prng_next() % HANG_N];
    int wlen = hang_strlen(word);
    char guessed[32];
    int g_count = 0;
    int wrong = 0;
    const int MAX_WRONG = 8;

    for (;;) {
        puts_both("\r\nWord: ");
        int i;
        for (i = 0; i < wlen; ++i) {
            int j, found = 0;
            for (j = 0; j < g_count; ++j) if (guessed[j] == word[i]) { found = 1; break; }
            putc_both(found ? word[i] : '_');
            putc_both(' ');
        }
        puts_both("  miss=");
        put_dec((uint32_t)wrong);
        puts_both("/");
        put_dec((uint32_t)MAX_WRONG);
        puts_both("\r\nLetter? ");
        char line[8];
        read_line(line, sizeof line);
        char c = line[0];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 'A' || c > 'Z') { puts_both("A-Z please\r\n"); continue; }
        if (g_count > 0) {
            int j, dup = 0;
            for (j = 0; j < g_count; ++j) if (guessed[j] == c) { dup = 1; break; }
            if (dup) { puts_both("already tried\r\n"); continue; }
        }
        guessed[g_count++] = c;
        if (!hang_contains(word, c)) ++wrong;
        /* check win */
        int win = 1;
        for (i = 0; i < wlen; ++i) {
            int j, found = 0;
            for (j = 0; j < g_count; ++j) if (guessed[j] == word[i]) { found = 1; break; }
            if (!found) { win = 0; break; }
        }
        if (win) { puts_both("\r\nWIN! word="); puts_both(word); puts_both("\r\n"); return; }
        if (wrong >= MAX_WRONG) {
            puts_both("\r\nLOSE. word=");
            puts_both(word);
            puts_both("\r\n");
            return;
        }
    }
}

/* --------- matrix rain --------- */
void cmd_matrix(void) {
    const int COLS = 76;
    const int ROWS = 22;
    int8_t pos[80];
    int i, frame;
    for (i = 0; i < COLS; ++i) pos[i] = -(int8_t)(prng_next() % 32);

    puts_both("\r\n(press any key to stop)\r\n");
    for (frame = 0; frame < 800; ++frame) {
        /* new line: draw chars at each col's current pos */
        for (i = 0; i < COLS; ++i) {
            if (pos[i] >= 0 && pos[i] < ROWS) {
                putc_both((char)(33 + (prng_next() % 90)));
            } else {
                putc_both(' ');
            }
            pos[i]++;
            if (pos[i] > ROWS + 5) pos[i] = -(int8_t)(prng_next() % 16);
        }
        puts_both("\r\n");
        /* delay + check key */
        uint32_t j;
        for (j = 0; j < 30000; ++j) __asm__ volatile("");
        if (uart_getc_nb() >= 0) break;
    }
}

/* --------- AI-vs-AI pong (text mode) --------- */
void cmd_pong(void) {
    const int W = 60, H = 18;
    int bx = W/2, by = H/2;
    int dx = 1, dy = 1;
    int pl = H/2, pr = H/2;          /* left/right paddle y center */
    int score_l = 0, score_r = 0;
    int frame;
    char buf[80];

    puts_both("\r\n(press any key to stop)\r\n");
    for (frame = 0; frame < 2000; ++frame) {
        /* AI tracks ball */
        if (pl < by) ++pl; else if (pl > by) --pl;
        if (pr < by) ++pr; else if (pr > by) --pr;

        /* move ball */
        bx += dx; by += dy;
        if (by <= 0)         { by = 0;   dy = 1;  }
        if (by >= H-1)       { by = H-1; dy = -1; }
        if (bx <= 1) {
            if (by >= pl-1 && by <= pl+1) dx = 1;
            else { ++score_r; bx = W/2; by = H/2; dx = 1; dy = (prng_next()&1)?1:-1; }
        }
        if (bx >= W-2) {
            if (by >= pr-1 && by <= pr+1) dx = -1;
            else { ++score_l; bx = W/2; by = H/2; dx = -1; dy = (prng_next()&1)?1:-1; }
        }

        /* render */
        puts_both("\x0C"); /* form feed (ignored on most picocom, but clears HDMI buf) */
        puts_both("Score  L:");
        put_dec((uint32_t)score_l);
        puts_both("  R:");
        put_dec((uint32_t)score_r);
        puts_both("\r\n");
        int y, x;
        for (y = 0; y < H; ++y) {
            for (x = 0; x < W; ++x) {
                char c = ' ';
                if (x == 0 || x == W-1)              c = '|';
                if (y == 0 || y == H-1)              c = '-';
                if (x == 1 && (y == pl-1||y==pl||y==pl+1)) c = '#';
                if (x == W-2 && (y == pr-1||y==pr||y==pr+1)) c = '#';
                if (x == bx && y == by)              c = 'O';
                buf[x] = c;
            }
            buf[W] = 0;
            puts_both(buf);
            puts_both("\r\n");
        }
        uint32_t j;
        for (j = 0; j < 60000; ++j) __asm__ volatile("");
        if (uart_getc_nb() >= 0) break;
    }
}

void cmd_mandel(void) {
    /* 8.8 fixed-point Mandelbrot, ASCII rendered. ~76x22 chars. */
    const int W = 76, H = 22;
    static const char ch[] = " .:-=+*#%@";
    int px, py, it;
    for (py = 0; py < H; ++py) {
        int32_t ci = -256 + (py * 512) / H;             /* -1.0 .. 1.0 in 8.8 */
        for (px = 0; px < W; ++px) {
            int32_t cr = -640 + (px * 1024) / W;        /* -2.5 .. 1.5 in 8.8 */
            int32_t zr = 0, zi = 0;
            for (it = 0; it < 20; ++it) {
                int32_t zr2 = (zr * zr) >> 8;
                int32_t zi2 = (zi * zi) >> 8;
                if (zr2 + zi2 > (4 << 8)) break;
                int32_t newzi = ((zr * zi) >> 7) + ci;
                zr = zr2 - zi2 + cr;
                zi = newzi;
            }
            putc_both((it >= 20) ? '@' : ch[(it * 9) / 20]);
        }
        puts_both("\r\n");
    }
}

static void cmd_clear(void) {
    /* For most VT terminals: ESC [ 2 J  ESC [ H.
       svo_term may not interpret these; also fill screen with spaces. */
    putc_both(0x0C);                                    /* form feed */
    for (int i = 0; i < 80 * 30; ++i) putc_both(' ');
    putc_both(0x0C);
}

static void jump_to_app(void) {
    void (*app)(void) = (void (*)(void))APP_BASE;
    app();
}

/* upload over UART, jumps on success. returns 1 on success, 0 on failure. */
static int do_upload(void) {
    puts_both("sz?");
    uint32_t size = read_u32_le();
    if (size == 0 || size > APP_MAX_SIZE) {
        puts_both(" ERR size=");
        put_hex8(size);
        puts_both("\r\n");
        return 0;
    }
    puts_both(" ok ");
    put_hex8(size);
    puts_both("\r\n");

    uint8_t *dst = (uint8_t *)APP_BASE;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < size; ++i) {
        uint8_t b = uart_getc_blocking();
        dst[i] = b;
        sum += b;
        if ((i & 0xFF) == 0) LED_REG = (i >> 8) & 0x3F;
    }
    uint32_t expected = read_u32_le();
    if (sum != expected) {
        puts_both("ERR crc got=");
        put_hex8(sum);
        puts_both(" want=");
        put_hex8(expected);
        puts_both("\r\n");
        return 0;
    }
    puts_both("OK run\r\n");
    LED_REG = 0;
    jump_to_app();
    puts_both("\r\napp returned\r\n");
    return 1;
}

static void cmd_dump(uint32_t addr, uint32_t len) {
    if (len > 256) len = 256;                            /* sanity cap */
    uint8_t *p = (uint8_t *)addr;
    for (uint32_t i = 0; i < len; i += 16) {
        put_hex8(addr + i);
        puts_both(": ");
        uint32_t row = len - i; if (row > 16) row = 16;
        for (uint32_t j = 0; j < 16; ++j) {
            if (j < row) { put_hex2(p[i + j]); putc_both(' '); }
            else         { puts_both("   "); }
        }
        puts_both("  ");
        for (uint32_t j = 0; j < row; ++j) {
            char c = (char)p[i + j];
            putc_both((c >= 0x20 && c < 0x7F) ? c : '.');
        }
        puts_both("\r\n");
    }
}

/* dispatch one line; returns 1 if user asked to upload (special path). */
static void dispatch(char *line) {
    const char *p = skip_spaces(line);
    if (!*p) return;

    /* legacy multi-char commands that contain '(' (e.g. cc puts(...)) must
       be matched BEFORE the interpreter, otherwise interp eats them. */
    if (tok_eq(p, "cc")) {
        while (*p && !is_space(*p)) ++p;
        while (*p && is_space(*p)) ++p;
        if (!*p) { puts_both("usage: cc <C source>\r\n"); return; }
        int n = cc_compile(p, CC_CODE_BASE, CC_CODE_MAX);
        if (n < 0) return;
        puts_both("[cc] ");
        put_dec((uint32_t)n);
        puts_both(" instr\r\n");
        cc_run(CC_CODE_BASE);
        puts_both("[cc] done\r\n");
        return;
    }

    /* try mini-C interpreter (let, print, printh, assign, bare expr) */
    if (interp_try(line)) return;

    if (tok_eq(p, "help") || tok_eq(p, "?")) { cmd_help(); return; }
    if (tok_eq(p, "clear") || tok_eq(p, "cls")) { cmd_clear(); return; }
    if (tok_eq(p, "mandel")) { cmd_mandel(); return; }
    if (tok_eq(p, "matrix")) { cmd_matrix(); return; }
    if (tok_eq(p, "pong"))   { cmd_pong();   return; }
    if (tok_eq(p, "guess"))  { cmd_guess();  return; }
    if (tok_eq(p, "hangman"))   { cmd_hangman(); return; }
    if (tok_eq(p, "ascii"))  { cmd_ascii();  return; }
    if (tok_eq(p, "info"))   { cmd_info();   return; }

    if (tok_eq(p, "u")) {                                /* upload */
        while (*p && !is_space(*p)) ++p;
        do_upload();
        return;
    }
    if (tok_eq(p, "g") || tok_eq(p, "run")) {
        puts_both("go\r\n");
        jump_to_app();
        puts_both("app returned\r\n");
        return;
    }

    /* commands with args */
    if (tok_eq(p, "led")) {
        while (*p && !is_space(*p)) ++p;
        uint32_t v;
        if (!parse_hex(&p, &v)) { puts_both("usage: led <hex>\r\n"); return; }
        LED_REG = v & 0x3F;
        puts_both("led=");
        put_hex2((uint8_t)(v & 0x3F));
        puts_both("\r\n");
        return;
    }
    if (tok_eq(p, "peek")) {
        while (*p && !is_space(*p)) ++p;
        uint32_t addr;
        if (!parse_hex(&p, &addr)) { puts_both("usage: peek <addr>\r\n"); return; }
        uint32_t v = *(volatile uint32_t *)(addr & ~3u);
        put_hex8(addr & ~3u); puts_both(": "); put_hex8(v); puts_both("\r\n");
        return;
    }
    if (tok_eq(p, "poke")) {
        while (*p && !is_space(*p)) ++p;
        uint32_t addr, val;
        if (!parse_hex(&p, &addr) || !parse_hex(&p, &val)) {
            puts_both("usage: poke <addr> <val>\r\n");
            return;
        }
        *(volatile uint32_t *)(addr & ~3u) = val;
        puts_both("ok\r\n");
        return;
    }
    if (tok_eq(p, "dump")) {
        while (*p && !is_space(*p)) ++p;
        uint32_t addr, len;
        if (!parse_hex(&p, &addr) || !parse_dec(&p, &len)) {
            puts_both("usage: dump <addr> <len>\r\n");
            return;
        }
        cmd_dump(addr, len);
        return;
    }
    if (tok_eq(p, "mul")) {
        while (*p && !is_space(*p)) ++p;
        uint32_t a, b;
        if (!parse_dec(&p, &a) || !parse_dec(&p, &b)) {
            puts_both("usage: mul <a> <b>\r\n");
            return;
        }
        put_dec(a * b); puts_both("\r\n");
        return;
    }
    if (tok_eq(p, "hex")) {
        while (*p && !is_space(*p)) ++p;
        uint32_t v;
        if (!parse_dec(&p, &v)) { puts_both("usage: hex <dec>\r\n"); return; }
        puts_both("0x"); put_hex8(v); puts_both("\r\n");
        return;
    }

    puts_both("unknown. try 'help'.\r\n");
}

/* --- stubs for features not yet implemented (referenced by gui.c/edit.c) --- */
void cmd_synth(void)   { puts_both("synth: not yet implemented\r\n"); }
void cmd_imgtest(void) { puts_both("imgtest: not yet implemented\r\n"); }
void cmd_img(void)     { puts_both("img: not yet implemented\r\n"); }
void cmd_cam(void)     { puts_both("cam: not yet implemented\r\n"); }

/* capture_* globals used by edit.c for modal CC output display */
int  capture_mode = 0;
char capture_buf[128];
int  capture_len  = 0;

static void shell_task(void) {
    gui_run();
    /* If gui_run ever returns, fall back to infinite loop. */
    for (;;) {}
}

static void led_task(void) {
    for (;;) {
        /* Burn cycles between toggles — timer IRQ may preempt us mid-loop. */
        for (volatile uint32_t i = 0; i < 500000; i++) { }
        /* Toggle LED bit 5 (highest user LED, bits 0-4 may be used elsewhere). */
        uint32_t v = LED_REG;
        v ^= 0x20;
        LED_REG = v;
    }
}

int main(void) {
    LED_REG = 0;
    cc_setup_syscalls();

    /* Splash screen */
    puts_both("\r\n\r\n");
    puts_both("  _   _        ___    ____\r\n");
    puts_both(" | \\ | |  ___ / _ \\  / ___|\r\n");
    puts_both(" |  \\| | / _ \\ | | | \\___ \\\r\n");
    puts_both(" | |\\  ||  __/ |_| |  ___) |\r\n");
    puts_both(" |_| \\_| \\___|\\___/  |____/\r\n");
    puts_both("\r\n");
    puts_both("       v0.2  picorv32 / Tang Nano 9K\r\n");
    puts_both("       32K BRAM  /  RV32IMC  /  HDMI\r\n");
    puts_both("       type 'help' for commands\r\n");
    puts_both("\r\n");

    task_create(shell_task);
    task_create(led_task);
    sched_start();
    return 0;  /* unreachable */
}
