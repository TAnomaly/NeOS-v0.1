/*
 * NeOS mini interpreter — C-like expression + statement evaluator.
 *
 * Grammar (single line, no { } yet, no functions defined by user):
 *
 *   stmt   := 'let' IDENT '=' expr          (declare/assign)
 *           | IDENT '=' expr                (assign existing or new)
 *           | 'print' expr                  (print as signed decimal)
 *           | 'printh' expr                 (print as 0xHHHHHHHH)
 *           | expr                          (auto-print result as decimal)
 *
 *   expr   := ternary (?: not yet)
 *           : or
 *   or     := and ('||' and)*
 *   and    := bor ('&&' bor)*
 *   bor    := xor ('|' xor)*
 *   xor    := band ('^' band)*
 *   band   := eq ('&' eq)*
 *   eq     := rel (('=='|'!=') rel)*
 *   rel    := shift (('<'|'<='|'>'|'>=') shift)*
 *   shift  := add (('<<'|'>>') add)*
 *   add    := mul (('+'|'-') mul)*
 *   mul    := unary (('*'|'/'|'%') unary)*
 *   unary  := ('+'|'-'|'~'|'!') unary | primary
 *   primary:= NUMBER (dec or 0x hex) | IDENT | '(' expr ')' | call
 *   call   := IDENT '(' [expr (',' expr)*] ')'
 *
 * Built-in functions:
 *   led(v)         set LED bits (low 6 bits), returns v
 *   peek(addr)     32-bit word read
 *   poke(addr,v)   32-bit word write, returns v
 *   delay(ms)      busy wait ms milliseconds, returns ms
 *   read()         non-blocking UART byte, -1 if none
 *   write(b)       UART+HDMI byte out, returns b
 */

#include <stdint.h>
#include "interp.h"

/* --- prototypes from main.c --- */
void puts_both(const char *s);
void putc_both(char c);
void put_dec(uint32_t v);
void put_hex8(uint32_t v);
void uart_putc_raw(char c);
void term_putc_raw(char c);
int  uart_getc_nb(void);

#define UART_TX_DATA  (*(volatile uint32_t *)0x10000000)
#define UART_STATUS   (*(volatile uint32_t *)0x10000004)
#define UART_RX_DATA  (*(volatile uint32_t *)0x10000008)
#define LED_REG       (*(volatile uint32_t *)0x10000010)
#define TERM_DATA     (*(volatile uint32_t *)0x10000020)
#define AUDIO_FREQ    (*(volatile uint32_t *)0x10000030)

/* --- variable storage: a-z (lower) and A-Z (upper, separate) → 52 slots --- */
static int32_t  vars[52];
static uint8_t  var_defined[52];

static int var_idx(char c) {
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= 'A' && c <= 'Z') return 26 + (c - 'A');
    return -1;
}

/* --- error flag (set by parser/evaluator, propagated out) --- */
static int err_flag;
static void err(const char *msg) {
    if (!err_flag) { puts_both("err: "); puts_both(msg); puts_both("\r\n"); }
    err_flag = 1;
}

/* --- tokenizer / cursor --- */
static const char *cur;

static int is_space(char c) { return c == ' ' || c == '\t'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }

static void skip_ws(void) { while (*cur && is_space(*cur)) ++cur; }

/* peek next non-space char without consuming */
static char peek_c(void) { skip_ws(); return *cur; }

/* match a literal string (after skipping ws). consumes on match. */
static int match(const char *s) {
    skip_ws();
    const char *p = cur;
    while (*s) {
        if (*p != *s) return 0;
        ++p; ++s;
    }
    cur = p;
    return 1;
}

/* match a keyword (must be followed by non-alnum or end) */
static int match_kw(const char *kw) {
    skip_ws();
    const char *p = cur;
    while (*kw) {
        if (*p != *kw) return 0;
        ++p; ++kw;
    }
    if (is_alnum(*p)) return 0;
    cur = p;
    return 1;
}

/* parse identifier, returns first char (we only support single-letter vars in
   this minimal version, multi-char names rejected for vars but accepted for
   builtin function names via separate path). */
static int parse_ident_buf(char *buf, int max) {
    skip_ws();
    if (!is_alpha(*cur)) return 0;
    int n = 0;
    while (is_alnum(*cur) && n < max - 1) buf[n++] = *cur++;
    buf[n] = '\0';
    return n;
}

/* --- forward --- */
static int32_t parse_expr(void);

/* --- delay loop, ~ms milliseconds at 27 MHz --- */
static void delay_ms(uint32_t ms) {
    /* inner loop is 3-4 instructions, ~27000 iters/ms is rough */
    for (uint32_t i = 0; i < ms; ++i)
        for (volatile uint32_t j = 0; j < 2700; ++j) ;
}

static int32_t call_builtin(const char *name, int32_t *args, int nargs) {
    /* simple strcmp inline */
    #define EQ1(a)            (name[0]==a[0]&&name[1]==0)
    #define EQ_(s)            (str_eq(name,s))
    /* use a local strcmp */
    /* led(v) */
    static const char N_led[]   = "led";
    static const char N_peek[]  = "peek";
    static const char N_poke[]  = "poke";
    static const char N_delay[] = "delay";
    static const char N_read[]  = "read";
    static const char N_write[] = "write";
    static const char N_tone[]  = "tone";
    int (*streq)(const char*, const char*);
    /* lambda not available; inline */
    #define SAME(s) ({ int _i=0; while (s[_i]&&name[_i]&&s[_i]==name[_i]) _i++; (s[_i]==0 && name[_i]==0); })

    if (SAME(N_led)) {
        if (nargs != 1) { err("led(v) wants 1 arg"); return 0; }
        LED_REG = (uint32_t)args[0] & 0x3F;
        return args[0];
    }
    if (SAME(N_peek)) {
        if (nargs != 1) { err("peek(addr) wants 1 arg"); return 0; }
        uint32_t a = (uint32_t)args[0] & ~3u;
        return (int32_t)(*(volatile uint32_t *)a);
    }
    if (SAME(N_poke)) {
        if (nargs != 2) { err("poke(a,v) wants 2 args"); return 0; }
        uint32_t a = (uint32_t)args[0] & ~3u;
        *(volatile uint32_t *)a = (uint32_t)args[1];
        return args[1];
    }
    if (SAME(N_delay)) {
        if (nargs != 1) { err("delay(ms) wants 1 arg"); return 0; }
        if (args[0] > 0) delay_ms((uint32_t)args[0]);
        return args[0];
    }
    if (SAME(N_read)) {
        if (nargs != 0) { err("read() takes no args"); return 0; }
        return (int32_t)uart_getc_nb();
    }
    if (SAME(N_write)) {
        if (nargs != 1) { err("write(b) wants 1 arg"); return 0; }
        char c = (char)(args[0] & 0xFF);
        uart_putc_raw(c);
        term_putc_raw(c);
        return args[0];
    }
    if (SAME(N_tone)) {
        if (nargs != 1) { err("tone(hz) wants 1 arg"); return 0; }
        uint32_t f = (uint32_t)args[0];
        if (f > 65535u) f = 65535u;
        AUDIO_FREQ = f;
        return args[0];
    }

    err("unknown function");
    (void)streq;
    return 0;
    #undef EQ1
    #undef EQ_
    #undef SAME
}

/* parse number — decimal or 0x hex */
static int32_t parse_number(void) {
    skip_ws();
    uint32_t v = 0;
    if (cur[0] == '0' && (cur[1] == 'x' || cur[1] == 'X')) {
        cur += 2;
        int n = 0;
        for (;;) {
            char c = *cur;
            int d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | (uint32_t)d;
            ++cur; ++n;
        }
        if (!n) { err("bad hex"); return 0; }
    } else {
        int n = 0;
        while (*cur >= '0' && *cur <= '9') {
            v = v * 10 + (uint32_t)(*cur - '0');
            ++cur; ++n;
        }
        if (!n) { err("bad number"); return 0; }
    }
    return (int32_t)v;
}

static int32_t parse_primary(void) {
    skip_ws();
    if (err_flag) return 0;

    if (*cur == '(') {
        ++cur;
        int32_t v = parse_expr();
        skip_ws();
        if (*cur != ')') { err("expected ')'"); return 0; }
        ++cur;
        return v;
    }

    if (is_digit(*cur)) return parse_number();

    if (is_alpha(*cur)) {
        char name[16];
        int n = parse_ident_buf(name, sizeof name);
        if (!n) { err("bad ident"); return 0; }
        skip_ws();
        if (*cur == '(') {
            /* function call */
            ++cur;
            int32_t args[4];
            int nargs = 0;
            skip_ws();
            if (*cur != ')') {
                for (;;) {
                    if (nargs >= 4) { err("too many args"); return 0; }
                    args[nargs++] = parse_expr();
                    if (err_flag) return 0;
                    skip_ws();
                    if (*cur == ',') { ++cur; continue; }
                    break;
                }
            }
            if (*cur != ')') { err("expected ')' in call"); return 0; }
            ++cur;
            return call_builtin(name, args, nargs);
        }
        /* variable read — must be single char */
        if (n != 1) { err("var name must be single letter"); return 0; }
        int vi = var_idx(name[0]);
        if (vi < 0 || !var_defined[vi]) { err("undef var"); return 0; }
        return vars[vi];
    }

    err("unexpected token");
    return 0;
}

static int32_t parse_unary(void) {
    skip_ws();
    if (*cur == '+') { ++cur; return parse_unary(); }
    if (*cur == '-') { ++cur; return -parse_unary(); }
    if (*cur == '~') { ++cur; return ~parse_unary(); }
    if (*cur == '!') { ++cur; return !parse_unary(); }
    return parse_primary();
}

static int32_t parse_mul(void) {
    int32_t a = parse_unary();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (*cur == '*' && cur[1] != '=') {
            ++cur; int32_t b = parse_unary(); a = a * b;
        } else if (*cur == '/' && cur[1] != '=' && cur[1] != '/') {
            ++cur; int32_t b = parse_unary();
            if (b == 0) { err("div by zero"); return 0; }
            a = a / b;
        } else if (*cur == '%' && cur[1] != '=') {
            ++cur; int32_t b = parse_unary();
            if (b == 0) { err("mod by zero"); return 0; }
            a = a % b;
        } else break;
    }
    return a;
}

static int32_t parse_add(void) {
    int32_t a = parse_mul();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (*cur == '+' && cur[1] != '=' && cur[1] != '+') { ++cur; a = a + parse_mul(); }
        else if (*cur == '-' && cur[1] != '=' && cur[1] != '-') { ++cur; a = a - parse_mul(); }
        else break;
    }
    return a;
}

static int32_t parse_shift(void) {
    int32_t a = parse_add();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (cur[0] == '<' && cur[1] == '<') { cur += 2; a = (int32_t)((uint32_t)a << parse_add()); }
        else if (cur[0] == '>' && cur[1] == '>') { cur += 2; a = (int32_t)((uint32_t)a >> parse_add()); }
        else break;
    }
    return a;
}

static int32_t parse_rel(void) {
    int32_t a = parse_shift();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if      (cur[0] == '<' && cur[1] == '=') { cur += 2; a = (a <= parse_shift()); }
        else if (cur[0] == '>' && cur[1] == '=') { cur += 2; a = (a >= parse_shift()); }
        else if (cur[0] == '<' && cur[1] != '<') { ++cur;    a = (a <  parse_shift()); }
        else if (cur[0] == '>' && cur[1] != '>') { ++cur;    a = (a >  parse_shift()); }
        else break;
    }
    return a;
}

static int32_t parse_eq(void) {
    int32_t a = parse_rel();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (cur[0] == '=' && cur[1] == '=') { cur += 2; a = (a == parse_rel()); }
        else if (cur[0] == '!' && cur[1] == '=') { cur += 2; a = (a != parse_rel()); }
        else break;
    }
    return a;
}

static int32_t parse_band(void) {
    int32_t a = parse_eq();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (cur[0] == '&' && cur[1] != '&' && cur[1] != '=') { ++cur; a = a & parse_eq(); }
        else break;
    }
    return a;
}

static int32_t parse_xor(void) {
    int32_t a = parse_band();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (cur[0] == '^' && cur[1] != '=') { ++cur; a = a ^ parse_band(); }
        else break;
    }
    return a;
}

static int32_t parse_bor(void) {
    int32_t a = parse_xor();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (cur[0] == '|' && cur[1] != '|' && cur[1] != '=') { ++cur; a = a | parse_xor(); }
        else break;
    }
    return a;
}

static int32_t parse_and(void) {
    int32_t a = parse_bor();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (cur[0] == '&' && cur[1] == '&') { cur += 2; int32_t b = parse_bor(); a = a && b; }
        else break;
    }
    return a;
}

static int32_t parse_or(void) {
    int32_t a = parse_and();
    for (;;) {
        if (err_flag) return 0;
        skip_ws();
        if (cur[0] == '|' && cur[1] == '|') { cur += 2; int32_t b = parse_and(); a = a || b; }
        else break;
    }
    return a;
}

static int32_t parse_expr(void) {
    return parse_or();
}

/* Print a signed 32-bit decimal value. */
static void print_dec_signed(int32_t v) {
    if (v < 0) { putc_both('-'); v = -v; }
    put_dec((uint32_t)v);
}

/* --- dispatch entry --- */
int interp_try(const char *line) {
    cur = line;
    err_flag = 0;
    skip_ws();
    if (!*cur) return 0;

    /* 'let' IDENT '=' expr */
    const char *save = cur;
    if (match_kw("let")) {
        skip_ws();
        if (!is_alpha(*cur) || (is_alnum(cur[1]) && cur[1] != ' ' && cur[1] != '=' && cur[1] != '\t')) {
            err("let: var name must be 1 letter");
            return 1;
        }
        int vi = var_idx(*cur);
        if (vi < 0) { err("let: invalid var name"); return 1; }
        ++cur;
        skip_ws();
        if (*cur != '=') { err("let: expected '='"); return 1; }
        ++cur;
        int32_t v = parse_expr();
        if (err_flag) return 1;
        skip_ws();
        if (*cur) { err("trailing junk"); return 1; }
        vars[vi] = v;
        var_defined[vi] = 1;
        putc_both((char)('a' + (vi < 26 ? vi : vi - 26 + 'A' - 'a')));
        puts_both(" = ");
        print_dec_signed(v);
        puts_both("\r\n");
        return 1;
    }
    cur = save;

    if (match_kw("print")) {
        skip_ws();
        int have_paren = 0;
        if (*cur == '(') { have_paren = 1; ++cur; }

        int first = 1;
        for (;;) {
            skip_ws();
            if (!*cur) break;
            if (have_paren && *cur == ')') break;

            if (!first) {
                /* expect comma */
                if (*cur != ',') { err("expected ','"); return 1; }
                ++cur;
                skip_ws();
            }
            first = 0;

            if (*cur == '"') {
                /* string literal */
                ++cur;
                while (*cur && *cur != '"') {
                    char c = *cur++;
                    if (c == '\\' && *cur) {
                        char e = *cur++;
                        switch (e) {
                            case 'n':  putc_both('\r'); putc_both('\n'); break;
                            case 'r':  putc_both('\r'); break;
                            case 't':  putc_both('\t'); break;
                            case '\\': putc_both('\\'); break;
                            case '"':  putc_both('"');  break;
                            case '0':  putc_both('\0'); break;
                            default:   putc_both(e);    break;
                        }
                    } else {
                        putc_both(c);
                    }
                }
                if (*cur != '"') { err("unterminated string"); return 1; }
                ++cur;
            } else {
                int32_t v = parse_expr();
                if (err_flag) return 1;
                print_dec_signed(v);
            }
        }

        if (have_paren) {
            skip_ws();
            if (*cur != ')') { err("expected ')'"); return 1; }
            ++cur;
        }
        skip_ws();
        if (*cur) { err("trailing junk"); return 1; }
        puts_both("\r\n");
        return 1;
    }
    cur = save;

    if (match_kw("printh")) {
        int32_t v = parse_expr();
        if (err_flag) return 1;
        skip_ws();
        if (*cur) { err("trailing junk"); return 1; }
        puts_both("0x");
        put_hex8((uint32_t)v);
        puts_both("\r\n");
        return 1;
    }
    cur = save;

    /* IDENT '=' expr  (assignment)  — single-letter var only */
    if (is_alpha(*cur) && (cur[1] == ' ' || cur[1] == '=' || cur[1] == '\t')) {
        char name = *cur;
        const char *probe = cur + 1;
        while (*probe == ' ' || *probe == '\t') ++probe;
        if (*probe == '=' && probe[1] != '=') {
            int vi = var_idx(name);
            if (vi < 0) { err("invalid var name"); return 1; }
            cur = probe + 1;
            int32_t v = parse_expr();
            if (err_flag) return 1;
            skip_ws();
            if (*cur) { err("trailing junk"); return 1; }
            vars[vi] = v;
            var_defined[vi] = 1;
            putc_both(name);
            puts_both(" = ");
            print_dec_signed(v);
            puts_both("\r\n");
            return 1;
        }
    }
    cur = save;

    /* Bare expression — heuristic to avoid eating legacy commands:
       trigger ONLY if first non-space char is digit / ( / unary op,
       OR the line contains a '(' (function call expression). */
    char c = *cur;
    int looks_expr =
        is_digit(c) || c == '(' || c == '-' || c == '+' || c == '~' || c == '!';
    if (!looks_expr) {
        /* check for '(' anywhere on the line — covers led(...), peek(...), etc. */
        const char *q = cur;
        int has_paren = 0;
        while (*q) { if (*q == '(') { has_paren = 1; break; } ++q; }
        if (!has_paren) return 0;     /* hand off to legacy dispatcher */
    }

    int32_t v = parse_expr();
    if (err_flag) return 1;
    skip_ws();
    if (*cur) {
        /* leftover token — not pure expression, hand off */
        return 0;
    }
    print_dec_signed(v);
    puts_both("\r\n");
    return 1;
}
