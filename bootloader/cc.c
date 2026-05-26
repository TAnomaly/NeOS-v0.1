/*
 * NeOS mini-C compiler v0.1
 *   Source -> RV32IM machine code into RAM, then jump and run.
 *
 * Tiny subset of C:
 *   - int variables, single-letter names (a-z, A-Z).  Storage at VARS_BASE.
 *   - statements: assignment, if, if/else, while, compound { ... }, expr;
 *   - operators: + - * / %  < <= > >= == !=  &  |  ^  << >>  ! - ~ (unary)
 *     (no && || short-circuit yet — use nested ifs)
 *   - builtin calls (as expressions or statements):
 *         led(n)         set LED bits low 6
 *         delay(n)       busy-wait ms
 *         peek(a)        read 32-bit word
 *         poke(a,v)      write 32-bit word
 *         print(n)       decimal int + newline (via NeOS put_dec)
 *         puts("...")    string literal + newline (via NeOS puts)
 *         getc()         non-blocking UART read, -1 if empty
 *
 * Code-gen strategy (very simple, not optimal):
 *   - Accumulator = a0.  Sub-expr push to stack, then pop into a1.
 *   - s0 holds VARS_BASE.
 *   - Builtins called via syscall table at SYSCALL_BASE.
 *   - Prelude sets up s0/sp; postlude RETs back to NeOS caller.
 */

#include <stdint.h>
#include "cc.h"

/* extern from main.c */
void puts_both(const char *s);
void putc_both(char c);
void put_dec(uint32_t v);
void put_hex8(uint32_t v);

/* ---- Addresses ---- */
#define VARS_BASE     0x00007000u
#define SYSCALL_BASE  0x00007080u   /* table of fn pointers */

/* Syscall indices */
enum {
    SC_LED   = 0,
    SC_DELAY = 1,
    SC_PEEK  = 2,
    SC_POKE  = 3,
    SC_PRINT = 4,
    SC_PUTS  = 5,
    SC_GETC  = 6,
    SC_MAX
};

/* ---- Error reporting ---- */
static int  err_flag;
static const char *err_pos;
static void cc_err(const char *msg) {
    if (err_flag) return;
    err_flag = 1;
    puts_both("cc err: ");
    puts_both(msg);
    puts_both("\r\n");
}

/* ---- Tokenizer ---- */
static const char *src_cur;
static int         tok;            /* current token kind */
static int32_t     tok_num;        /* if tok == TK_NUM */
static char        tok_id;         /* single-letter var name */
static const char *tok_str_start;  /* if tok == TK_STR */
static int         tok_str_len;

enum {
    TK_EOF=0, TK_NUM, TK_ID,
    TK_LPAREN='(', TK_RPAREN=')', TK_LBRACE='{', TK_RBRACE='}',
    TK_SEMI=';', TK_COMMA=',',
    TK_PLUS='+', TK_MINUS='-', TK_STAR='*', TK_SLASH='/', TK_PERCENT='%',
    TK_AMP='&', TK_PIPE='|', TK_CARET='^', TK_TILDE='~', TK_BANG='!',
    TK_ASSIGN='=',
    TK_LSHIFT=256, TK_RSHIFT, TK_EQ, TK_NE, TK_LE, TK_GE, TK_LT, TK_GT,
    TK_KW_INT, TK_KW_IF, TK_KW_ELSE, TK_KW_WHILE,
    TK_STR
};

static int is_alpha(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
static int is_digit(char c){return c>='0'&&c<='9';}
static int is_alnum(char c){return is_alpha(c)||is_digit(c);}
static int is_space(char c){return c==' '||c=='\t'||c=='\n'||c=='\r';}

static void skip_ws(void){
    for(;;){
        while(is_space(*src_cur)) ++src_cur;
        if(src_cur[0]=='/' && src_cur[1]=='/'){
            while(*src_cur && *src_cur!='\n') ++src_cur;
        } else break;
    }
}

static int kw_match(const char *kw, int n){
    int i; for(i=0;i<n;i++) if(src_cur[i]!=kw[i]) return 0;
    if(is_alnum(src_cur[n])) return 0;
    src_cur += n; return 1;
}

static void next_tok(void){
    skip_ws();
    err_pos = src_cur;
    char c = *src_cur;
    if(!c){tok=TK_EOF; return;}

    /* numbers */
    if(is_digit(c)){
        uint32_t v=0;
        if(c=='0' && (src_cur[1]=='x'||src_cur[1]=='X')){
            src_cur+=2;
            while(1){
                char d=*src_cur; int v2;
                if(d>='0'&&d<='9') v2=d-'0';
                else if(d>='a'&&d<='f') v2=d-'a'+10;
                else if(d>='A'&&d<='F') v2=d-'A'+10;
                else break;
                v=(v<<4)|v2; ++src_cur;
            }
        } else {
            while(is_digit(*src_cur)){v=v*10+(*src_cur-'0'); ++src_cur;}
        }
        tok=TK_NUM; tok_num=(int32_t)v; return;
    }

    /* string literal */
    if(c=='"'){
        ++src_cur;
        tok_str_start = src_cur;
        while(*src_cur && *src_cur!='"') ++src_cur;
        tok_str_len = src_cur - tok_str_start;
        if(*src_cur!='"'){cc_err("unterminated string"); tok=TK_EOF; return;}
        ++src_cur;
        tok=TK_STR; return;
    }

    /* keywords/identifiers */
    if(is_alpha(c)){
        if(kw_match("int",3)){tok=TK_KW_INT; return;}
        if(kw_match("if",2)){tok=TK_KW_IF; return;}
        if(kw_match("else",4)){tok=TK_KW_ELSE; return;}
        if(kw_match("while",5)){tok=TK_KW_WHILE; return;}
        /* known builtins / identifiers — accept multi-char identifiers,
           but we only allow single-letter vars elsewhere.  Builtin names
           handled as identifiers; the parser checks them by name from src. */
        tok_id = c;
        if(is_alnum(src_cur[1])){
            /* multi-char id: consume them, store start in tok_str_start */
            tok_str_start = src_cur;
            int n=0;
            while(is_alnum(*src_cur)){++src_cur; ++n;}
            tok_str_len = n;
        } else {
            ++src_cur;
            tok_str_start = src_cur - 1;
            tok_str_len = 1;
        }
        tok=TK_ID; return;
    }

    /* 2-char operators */
    if(c=='<' && src_cur[1]=='<'){src_cur+=2; tok=TK_LSHIFT; return;}
    if(c=='>' && src_cur[1]=='>'){src_cur+=2; tok=TK_RSHIFT; return;}
    if(c=='=' && src_cur[1]=='='){src_cur+=2; tok=TK_EQ; return;}
    if(c=='!' && src_cur[1]=='='){src_cur+=2; tok=TK_NE; return;}
    if(c=='<' && src_cur[1]=='='){src_cur+=2; tok=TK_LE; return;}
    if(c=='>' && src_cur[1]=='='){src_cur+=2; tok=TK_GE; return;}
    if(c=='<'){++src_cur; tok=TK_LT; return;}
    if(c=='>'){++src_cur; tok=TK_GT; return;}

    /* single-char operators / punctuation */
    ++src_cur;
    tok = (int)(uint8_t)c;
}

/* ---- Code buffer ---- */
static uint32_t *code_buf;
static int       code_pos;
static int       code_max;

static void emit(uint32_t w){
    if(code_pos >= code_max){cc_err("code buffer overflow"); return;}
    code_buf[code_pos++] = w;
}

/* ---- RISC-V encoders ----
   reg numbers we use:
     x0=zero, x1=ra, x2=sp, x5=t0, x6=t1, x7=t2,
     x8=s0 (VARS_BASE), x10=a0, x11=a1, x12=a2,
     x28..x31 = t3..t6
*/
#define ZERO 0
#define RA   1
#define SP   2
#define T0   5
#define T1   6
#define T2   7
#define S0   8
#define S1   9
#define A0   10
#define A1   11
#define A2   12

/* R-type: funct7|rs2|rs1|funct3|rd|opcode */
static uint32_t r_type(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op){
    return (f7<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|(rd<<7)|op;
}
/* I-type: imm[11:0]|rs1|funct3|rd|opcode */
static uint32_t i_type(int32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op){
    return ((imm & 0xFFF) << 20)|(rs1<<15)|(f3<<12)|(rd<<7)|op;
}
/* S-type: imm[11:5]|rs2|rs1|funct3|imm[4:0]|opcode */
static uint32_t s_type(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op){
    uint32_t i = imm & 0xFFF;
    return ((i>>5)<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)|((i&0x1F)<<7)|op;
}
/* B-type: imm[12|10:5|4:1|11] bits arranged. */
static uint32_t b_type(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t op){
    /* imm is byte offset, must be even, range [-4096, 4094] */
    uint32_t bit12 = (imm >> 12) & 1;
    uint32_t bit11 = (imm >> 11) & 1;
    uint32_t bits10_5 = (imm >> 5) & 0x3F;
    uint32_t bits4_1  = (imm >> 1) & 0xF;
    return (bit12<<31)|(bits10_5<<25)|(rs2<<20)|(rs1<<15)|(f3<<12)
         | (bits4_1<<8)|(bit11<<7)|op;
}
/* U-type: imm[31:12]|rd|opcode */
static uint32_t u_type(int32_t imm, uint32_t rd, uint32_t op){
    return (imm & 0xFFFFF000) | (rd<<7) | op;
}
/* J-type for JAL: imm[20|10:1|11|19:12]|rd|opcode */
static uint32_t j_type(int32_t imm, uint32_t rd, uint32_t op){
    uint32_t bit20    = (imm >> 20) & 1;
    uint32_t bits10_1 = (imm >>  1) & 0x3FF;
    uint32_t bit11    = (imm >> 11) & 1;
    uint32_t bits19_12= (imm >> 12) & 0xFF;
    return (bit20<<31)|(bits10_1<<21)|(bit11<<20)|(bits19_12<<12)|(rd<<7)|op;
}

/* Instructions used */
static void emit_addi (uint32_t rd, uint32_t rs1, int32_t imm){ emit(i_type(imm, rs1, 0, rd, 0x13)); }
static void emit_lui  (uint32_t rd, uint32_t imm20){ emit(u_type(imm20<<12, rd, 0x37)); }
static void emit_add  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0,  rs2, rs1, 0, rd, 0x33)); }
static void emit_sub  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0x20,rs2,rs1, 0, rd, 0x33)); }
static void emit_mul  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(1,  rs2, rs1, 0, rd, 0x33)); }
static void emit_div  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(1,  rs2, rs1, 4, rd, 0x33)); }
static void emit_rem  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(1,  rs2, rs1, 6, rd, 0x33)); }
static void emit_and_ (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0,  rs2, rs1, 7, rd, 0x33)); }
static void emit_or_  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0,  rs2, rs1, 6, rd, 0x33)); }
static void emit_xor_ (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0,  rs2, rs1, 4, rd, 0x33)); }
static void emit_sll  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0,  rs2, rs1, 1, rd, 0x33)); }
static void emit_srl  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0,  rs2, rs1, 5, rd, 0x33)); }
static void emit_slt  (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0,  rs2, rs1, 2, rd, 0x33)); }
static void emit_sltu (uint32_t rd, uint32_t rs1, uint32_t rs2){ emit(r_type(0,  rs2, rs1, 3, rd, 0x33)); }
static void emit_lw   (uint32_t rd, uint32_t rs1, int32_t imm){ emit(i_type(imm, rs1, 2, rd, 0x03)); }
static void emit_sw   (uint32_t rs2, uint32_t rs1, int32_t imm){ emit(s_type(imm, rs2, rs1, 2, 0x23)); }
static void emit_beq  (uint32_t rs1, uint32_t rs2, int32_t imm){ emit(b_type(imm, rs2, rs1, 0, 0x63)); }
static void emit_bne  (uint32_t rs1, uint32_t rs2, int32_t imm){ emit(b_type(imm, rs2, rs1, 1, 0x63)); }
static void emit_jal  (uint32_t rd, int32_t imm){ emit(j_type(imm, rd, 0x6F)); }
static void emit_jalr (uint32_t rd, uint32_t rs1, int32_t imm){ emit(i_type(imm, rs1, 0, rd, 0x67)); }
static void emit_ret  (void){ emit_jalr(ZERO, RA, 0); }

/* li rd, imm32 (always 2 instructions: LUI + ADDI). Works for any 32-bit. */
static void emit_li(uint32_t rd, uint32_t imm){
    uint32_t lo = imm & 0xFFF;
    uint32_t hi = (imm + ((lo & 0x800) ? 0x1000 : 0)) >> 12;
    /* If hi == 0, single ADDI suffices but we keep simple */
    if (hi == 0 && (lo & 0x800) == 0) {
        emit_addi(rd, ZERO, (int32_t)lo);
    } else {
        emit_lui(rd, hi);
        if (lo) emit_addi(rd, rd, (int32_t)(lo | ((lo & 0x800) ? 0xFFFFF000 : 0)));
    }
}

/* Push a0 onto stack:  addi sp,sp,-4 ; sw a0, 0(sp) */
static void push_a0(void){ emit_addi(SP, SP, -4); emit_sw(A0, SP, 0); }
/* Pop into a1:  lw a1, 0(sp) ; addi sp,sp,4 */
static void pop_a1(void){ emit_lw(A1, SP, 0); emit_addi(SP, SP, 4); }

/* Call syscall index sc_idx via syscall table.  Args already in a0[,a1]. */
static void emit_syscall(int sc_idx){
    emit_li(T0, SYSCALL_BASE + 4*sc_idx);
    emit_lw(T0, T0, 0);          /* T0 = *(SYSCALL_BASE+4*idx) */
    emit_jalr(RA, T0, 0);
}

/* ---- Parser forwards ---- */
static void parse_expr(void);
static void parse_stmt(void);

static int accept(int t){ if(tok==t){next_tok(); return 1;} return 0; }
static void expect(int t, const char *what){
    if(!accept(t)){ cc_err(what); }
}

/* ---- Expression grammar (precedence low -> high) ----
   assign := bor ('=' assign)?
   bor    := xor ('|' xor)*
   xor    := band ('^' band)*
   band   := eq ('&' eq)*
   eq     := rel (('=='|'!=') rel)*
   rel    := shift (('<'|'<='|'>'|'>=') shift)*
   shift  := add (('<<'|'>>') add)*
   add    := mul (('+'|'-') mul)*
   mul    := unary (('*'|'/'|'%') unary)*
   unary  := ('-'|'!'|'~') unary | primary
   primary:= NUM | ID | ID '(' args ')' | '(' expr ')' | STR-only-allowed-in-puts
*/

static void parse_assign(void);
static void parse_primary(void);

/* a0 = b ? 1 : 0 where b = (a0 == 0) etc */
static void cmp_to_bool(int code){
    /* code: 0=lt,1=ge,2=eq,3=ne,4=le,5=gt */
    /* a1 = pop, a0 = top.  After: a0 = (a0 OP a1) ? 1 : 0 */
    pop_a1();
    /* Convention used by caller: stack had LHS first then RHS computed in a0.
       So actually LHS is in a1 (popped), RHS in a0.  We compare LHS OP RHS. */
    /* swap: x = a1, y = a0.  We want x OP y. */
    switch(code){
        case 0: emit_slt(A0, A1, A0); break;                  /* x<y */
        case 5: emit_slt(A0, A0, A1); break;                  /* y<x  ↔ x>y */
        case 1: /* x>=y → !(x<y) */
            emit_slt(A0, A1, A0); emit_xor_(A0, A0, ZERO);
            emit_addi(A0, ZERO, 1); /* placeholder */ break;
        case 4: /* x<=y → !(y<x) */
            emit_slt(A0, A0, A1); emit_xor_(A0, A0, ZERO);
            emit_addi(A0, ZERO, 1); break;
        case 2: /* x==y → sltu (x^y, 1) */
            emit_xor_(A0, A1, A0);
            emit_sltu(A0, ZERO, A0); /* a0 = (0 < a0) ? 1 : 0 = (a0!=0)?1:0 */
            emit_xor_(A0, A0, ZERO); /* placeholder no-op */
            emit_addi(A0, A0, 0);    /* keep */
            /* Now a0 = (x!=y)?1:0.  Invert: a0 = 1 - a0 */
            { /* a0 = 1 - a0 → addi t0,zero,1; sub a0,t0,a0 */
              emit_addi(T0, ZERO, 1); emit_sub(A0, T0, A0);
            }
            break;
        case 3: /* x!=y */
            emit_xor_(A0, A1, A0);
            emit_sltu(A0, ZERO, A0); /* (a0!=0)?1:0 */
            break;
    }
}

/* Simpler cmp_to_bool: rewrite using boolean conventions.
   Above got messy. Replace with cleaner per-case codegen. */
static void cmp_op(int kind){
    /* kind: '<' '>' '=' (eq) '!' (ne) 'l' (le) 'g' (ge) */
    pop_a1();  /* a1 = LHS, a0 = RHS */
    switch(kind){
        case '<': emit_slt(A0, A1, A0); return;        /* a0 = LHS<RHS */
        case '>': emit_slt(A0, A0, A1); return;        /* a0 = RHS<LHS */
        case '=': {
            emit_xor_(A0, A1, A0);
            emit_sltu(A0, ZERO, A0);  /* (LHS^RHS)!=0 → 1 */
            emit_addi(T0, ZERO, 1); emit_sub(A0, T0, A0);  /* a0 = 1 - a0 */
            return;
        }
        case '!': {
            emit_xor_(A0, A1, A0);
            emit_sltu(A0, ZERO, A0);
            return;
        }
        case 'l': {
            /* LHS<=RHS  ↔  !(RHS<LHS) */
            emit_slt(A0, A0, A1);
            emit_addi(T0, ZERO, 1); emit_sub(A0, T0, A0);
            return;
        }
        case 'g': {
            /* LHS>=RHS  ↔  !(LHS<RHS) */
            emit_slt(A0, A1, A0);
            emit_addi(T0, ZERO, 1); emit_sub(A0, T0, A0);
            return;
        }
    }
}

/* ---- Builtins by name (multi-char identifier match) ---- */
static int id_eq(const char *s){
    int i;
    for(i=0;i<tok_str_len;i++) if(tok_str_start[i]!=s[i]) return 0;
    return s[tok_str_len]=='\0';
}

static void parse_call_args(int nargs){
    /* Caller already consumed '('.  Parse nargs comma-separated exprs.
       1st arg result kept in a0.  For multi-arg, push a0 between, then pop
       into a1/a2 in correct order. */
    int i;
    for(i=0;i<nargs;i++){
        parse_assign();
        if(err_flag) return;
        if(i==0){
            if(nargs>1) push_a0();
        } else if (i==1){
            /* a0 = arg1; a1 (target) = pop arg0 */
            emit_addi(A1, A0, 0);  /* a1 = a0 */
            pop_a1();              /* a1 = pop → arg0 */
            /* swap a0 and a1 so a0=arg0, a1=arg1 */
            emit_addi(A2, A0, 0);  /* a2 = a1's old value (now in a0?) */
            /* Wait: after pop_a1, a1 holds arg0 (the popped value).  We want
               a0=arg0, a1=arg1.  Currently a0=arg1, a1=arg0. Swap: */
            emit_addi(T0, A0, 0);
            emit_addi(A0, A1, 0);
            emit_addi(A1, T0, 0);
        }
        if(i<nargs-1){
            expect(TK_COMMA,"expected ,");
            if(err_flag) return;
        }
    }
    expect(TK_RPAREN,"expected )");
}

/* Parse builtin call. Returns 1 if handled, 0 if name unknown. */
static int parse_builtin(void){
    /* Caller has parsed identifier, then sees '(' next */
    if(!is_alpha(tok_str_start[0])) return 0;
    int sc = -1;
    int nargs = 0;
    int is_puts = 0;

    if(id_eq("led"))   { sc=SC_LED;   nargs=1; }
    else if(id_eq("delay")){sc=SC_DELAY;nargs=1;}
    else if(id_eq("peek")){sc=SC_PEEK; nargs=1;}
    else if(id_eq("poke")){sc=SC_POKE; nargs=2;}
    else if(id_eq("print")){sc=SC_PRINT;nargs=1;}
    else if(id_eq("puts")){sc=SC_PUTS; nargs=1; is_puts=1;}
    else if(id_eq("getc")){sc=SC_GETC; nargs=0;}
    else return 0;

    next_tok();              /* consume '(' (was checked by caller) */

    if(nargs==0){
        expect(TK_RPAREN,"expected )");
    } else if(is_puts){
        /* puts expects a string literal */
        if(tok != TK_STR){ cc_err("puts wants \"string\""); return 1; }
        /* Emit string into code stream after a forward jump, then point
           a0 at the string and call. */
        int str_len = tok_str_len;
        next_tok();
        expect(TK_RPAREN,"expected )");
        /* Layout:
              jal x0, after_str            ; skip over string bytes
            str_label:
              .byte ...
              .byte 0
            after_str:
              auipc a0, 0                  ; a0 = current PC
              addi  a0, a0, -offset_to_str ; back-patch
              syscall puts
        */
        int jal_pos = code_pos;
        emit_jal(ZERO, 0);                 /* fix offset later */
        int str_pos = code_pos;            /* word index where string starts */
        /* Write string bytes packed 4 per word */
        int wcount = (str_len + 1 + 3) / 4;     /* +1 for NUL, round up to word */
        int i;
        for(i=0; i<wcount; i++){
            uint32_t w = 0;
            int j;
            for(j=0; j<4; j++){
                int idx = i*4 + j;
                uint8_t b = 0;
                if(idx < str_len) b = (uint8_t)tok_str_start[idx];
                else if(idx == str_len) b = 0;
                w |= ((uint32_t)b) << (j*8);
            }
            emit(w);
        }
        int after_str_pos = code_pos;
        /* Patch jal at jal_pos to jump to after_str_pos */
        int32_t jal_off = (after_str_pos - jal_pos) * 4;
        code_buf[jal_pos] = j_type(jal_off, ZERO, 0x6F);
        /* AUIPC a0, 0 ; addi a0, a0, str_addr - pc */
        int auipc_pos = code_pos;
        emit(u_type(0, A0, 0x17));         /* AUIPC a0, 0 (no hi) */
        /* offset from auipc_pos to str_pos */
        int32_t back = (str_pos - auipc_pos) * 4;
        emit_addi(A0, A0, back);
        /* syscall puts */
        emit_syscall(SC_PUTS);
        return 1;
    } else {
        parse_call_args(nargs);
        if(err_flag) return 1;
        emit_syscall(sc);
    }
    return 1;
}

/* ---- Primary ---- */
static void parse_primary(void){
    if(tok==TK_NUM){
        emit_li(A0, (uint32_t)tok_num);
        next_tok();
        return;
    }
    if(tok==TK_LPAREN){
        next_tok();
        parse_assign();
        expect(TK_RPAREN,"expected )");
        return;
    }
    if(tok==TK_ID){
        /* function call? */
        const char *saved_start = tok_str_start;
        int saved_len = tok_str_len;
        char saved_c = tok_id;
        next_tok();
        if(tok==TK_LPAREN){
            /* restore identifier context for parse_builtin name match */
            tok_str_start = saved_start;
            tok_str_len = saved_len;
            tok_id = saved_c;
            if(!parse_builtin()) cc_err("unknown function");
            return;
        }
        /* variable read */
        if(saved_len != 1){ cc_err("var name must be single letter"); return; }
        int vi = (saved_c>='a'&&saved_c<='z') ? saved_c-'a'
               : (saved_c>='A'&&saved_c<='Z') ? saved_c-'A'+26 : -1;
        if(vi<0){ cc_err("bad var"); return; }
        /* a0 = lw vi*4(s0) */
        emit_lw(A0, S0, vi*4);
        return;
    }
    cc_err("expected expression");
    next_tok();
}

static void parse_unary(void){
    if(tok==TK_MINUS){ next_tok(); parse_unary(); emit_sub(A0, ZERO, A0); return; }
    if(tok==TK_BANG){  next_tok(); parse_unary();
                       emit_sltu(A0, ZERO, A0);
                       emit_addi(T0, ZERO, 1); emit_sub(A0, T0, A0); return; }
    if(tok==TK_TILDE){ next_tok(); parse_unary();
                       emit_addi(T0, ZERO, -1); emit_xor_(A0, A0, T0); return; }
    parse_primary();
}

static void parse_mul(void){
    parse_unary();
    while(!err_flag){
        if(tok==TK_STAR){next_tok(); push_a0(); parse_unary(); pop_a1(); emit_mul(A0, A1, A0); }
        else if(tok==TK_SLASH){next_tok(); push_a0(); parse_unary(); pop_a1(); emit_div(A0, A1, A0); }
        else if(tok==TK_PERCENT){next_tok(); push_a0(); parse_unary(); pop_a1(); emit_rem(A0, A1, A0); }
        else break;
    }
}
static void parse_add(void){
    parse_mul();
    while(!err_flag){
        if(tok==TK_PLUS){ next_tok(); push_a0(); parse_mul(); pop_a1(); emit_add(A0, A1, A0); }
        else if(tok==TK_MINUS){ next_tok(); push_a0(); parse_mul(); pop_a1(); emit_sub(A0, A1, A0); }
        else break;
    }
}
static void parse_shift(void){
    parse_add();
    while(!err_flag){
        if(tok==TK_LSHIFT){next_tok(); push_a0(); parse_add(); pop_a1(); emit_sll(A0, A1, A0);}
        else if(tok==TK_RSHIFT){next_tok(); push_a0(); parse_add(); pop_a1(); emit_srl(A0, A1, A0);}
        else break;
    }
}
static void parse_rel(void){
    parse_shift();
    while(!err_flag){
        if(tok==TK_LT){next_tok(); push_a0(); parse_shift(); cmp_op('<');}
        else if(tok==TK_GT){next_tok(); push_a0(); parse_shift(); cmp_op('>');}
        else if(tok==TK_LE){next_tok(); push_a0(); parse_shift(); cmp_op('l');}
        else if(tok==TK_GE){next_tok(); push_a0(); parse_shift(); cmp_op('g');}
        else break;
    }
}
static void parse_eq(void){
    parse_rel();
    while(!err_flag){
        if(tok==TK_EQ){next_tok(); push_a0(); parse_rel(); cmp_op('=');}
        else if(tok==TK_NE){next_tok(); push_a0(); parse_rel(); cmp_op('!');}
        else break;
    }
}
static void parse_band(void){
    parse_eq();
    while(!err_flag){
        if(tok==TK_AMP){next_tok(); push_a0(); parse_eq(); pop_a1(); emit_and_(A0, A1, A0);}
        else break;
    }
}
static void parse_xor(void){
    parse_band();
    while(!err_flag){
        if(tok==TK_CARET){next_tok(); push_a0(); parse_band(); pop_a1(); emit_xor_(A0, A1, A0);}
        else break;
    }
}
static void parse_bor(void){
    parse_xor();
    while(!err_flag){
        if(tok==TK_PIPE){next_tok(); push_a0(); parse_xor(); pop_a1(); emit_or_(A0, A1, A0);}
        else break;
    }
}

/* assignment: parse_assign for `var = expr` else fall through */
static void parse_assign(void){
    /* check if this looks like an assignment: ID '=' (not '==') */
    if(tok==TK_ID){
        const char *save_src = src_cur;
        int save_tok = tok;
        const char *save_str = tok_str_start;
        int save_len = tok_str_len;
        char save_c = tok_id;
        /* peek next */
        next_tok();
        if(tok==TK_ASSIGN){
            /* assignment! */
            if(save_len != 1){ cc_err("var name must be single letter"); return; }
            int vi = (save_c>='a'&&save_c<='z') ? save_c-'a'
                   : (save_c>='A'&&save_c<='Z') ? save_c-'A'+26 : -1;
            if(vi<0){ cc_err("bad var"); return; }
            next_tok();      /* consume '=' */
            parse_assign();
            /* sw a0, vi*4(s0) */
            emit_sw(A0, S0, vi*4);
            return;
        }
        /* not assignment, restore */
        src_cur = save_src; tok = save_tok;
        tok_str_start = save_str; tok_str_len = save_len; tok_id = save_c;
    }
    parse_bor();
}
static void parse_expr(void){ parse_assign(); }

/* ---- Statements ---- */

/* parse a single stmt; emit code. */
static void parse_stmt(void){
    if(err_flag) return;

    if(accept(TK_LBRACE)){
        while(!err_flag && tok!=TK_RBRACE && tok!=TK_EOF) parse_stmt();
        expect(TK_RBRACE,"expected }");
        return;
    }
    if(accept(TK_KW_INT)){
        /* int x = expr;  (declaration with init — we treat as assignment;
                            no scoping, all vars are global) */
        if(tok != TK_ID || tok_str_len != 1){
            cc_err("expected single-letter var after int"); return;
        }
        char name = tok_id;
        next_tok();
        if(accept(TK_ASSIGN)){
            parse_expr();
            int vi = (name>='a'&&name<='z') ? name-'a'
                   : (name>='A'&&name<='Z') ? name-'A'+26 : -1;
            if(vi<0){cc_err("bad var"); return;}
            emit_sw(A0, S0, vi*4);
        }
        expect(TK_SEMI,"expected ;");
        return;
    }
    if(accept(TK_KW_IF)){
        expect(TK_LPAREN,"expected (");
        parse_expr();
        expect(TK_RPAREN,"expected )");
        /* if (a0==0) jump to else/end */
        int bz_pos = code_pos;
        emit(b_type(0, ZERO, A0, 0, 0x63)); /* BEQ a0, zero, ?? (patch later) */
        parse_stmt();
        int after_then = code_pos;
        if(accept(TK_KW_ELSE)){
            /* jal x0, end; patch bz to skip-to-else */
            int jmp_end_pos = code_pos;
            emit_jal(ZERO, 0);
            int else_pos = code_pos;
            int32_t off = (else_pos - bz_pos) * 4;
            code_buf[bz_pos] = b_type(off, ZERO, A0, 0, 0x63);
            parse_stmt();
            int end_pos = code_pos;
            int32_t end_off = (end_pos - jmp_end_pos) * 4;
            code_buf[jmp_end_pos] = j_type(end_off, ZERO, 0x6F);
        } else {
            int32_t off = (after_then - bz_pos) * 4;
            code_buf[bz_pos] = b_type(off, ZERO, A0, 0, 0x63);
        }
        return;
    }
    if(accept(TK_KW_WHILE)){
        int loop_top = code_pos;
        expect(TK_LPAREN,"expected (");
        parse_expr();
        expect(TK_RPAREN,"expected )");
        int bz_pos = code_pos;
        emit(b_type(0, ZERO, A0, 0, 0x63));
        parse_stmt();
        int32_t back = (loop_top - code_pos) * 4;
        emit_jal(ZERO, back);
        int end = code_pos;
        int32_t off = (end - bz_pos) * 4;
        code_buf[bz_pos] = b_type(off, ZERO, A0, 0, 0x63);
        return;
    }
    if(accept(TK_SEMI)) return;                /* empty stmt */

    /* expression statement */
    parse_expr();
    expect(TK_SEMI,"expected ;");
}

/* ---- Top-level ---- */
int cc_compile(const char *src, uint32_t *code_base, int max_words){
    src_cur  = src;
    err_flag = 0;
    code_buf = code_base;
    code_pos = 0;
    code_max = max_words;

    /* Prelude:
         addi sp, sp, -16     ; save ra
         sw   ra, 12(sp)
         li   s0, VARS_BASE   ; var base
    */
    emit_addi(SP, SP, -16);
    emit_sw(RA, SP, 12);
    emit_li(S0, VARS_BASE);

    next_tok();
    while(!err_flag && tok != TK_EOF){
        parse_stmt();
    }

    /* Postlude:
         lw   ra, 12(sp)
         addi sp, sp, 16
         ret
    */
    emit_lw(RA, SP, 12);
    emit_addi(SP, SP, 16);
    emit_ret();

    if(err_flag){
        puts_both("  at: ");
        const char *p = err_pos;
        int n=0;
        while(*p && *p!='\n' && n<32){ putc_both(*p++); ++n; }
        puts_both("\r\n");
        return -1;
    }
    return code_pos;
}

void cc_run(uint32_t *code_base){
    void (*fn)(void) = (void(*)(void))code_base;
    fn();
}

/* ---- Syscall wrappers exposed to compiled code via SYSCALL table ---- */
#define LED_REG_ADDR   ((volatile uint32_t *)0x10000010)

extern int uart_getc_nb(void);

static void sys_led(uint32_t v){ *LED_REG_ADDR = (~v) & 0x3F; }
static void sys_delay(uint32_t ms){
    uint32_t i,j;
    for(i=0;i<ms;i++) for(j=0;j<2700;j++) __asm__ volatile("");
}
static uint32_t sys_peek(uint32_t a){ return *(volatile uint32_t *)(a & ~3u); }
static void sys_poke(uint32_t a, uint32_t v){ *(volatile uint32_t *)(a & ~3u) = v; }
static void sys_print(int32_t v){
    if(v<0){putc_both('-'); v=-v;}
    put_dec((uint32_t)v);
    putc_both('\r'); putc_both('\n');
}
static void sys_puts(const char *s){ puts_both(s); puts_both("\r\n"); }
static int  sys_getc(void){ return uart_getc_nb(); }

void cc_setup_syscalls(void){
    volatile uint32_t *t = (volatile uint32_t *)SYSCALL_BASE;
    t[SC_LED]   = (uint32_t)(uintptr_t)sys_led;
    t[SC_DELAY] = (uint32_t)(uintptr_t)sys_delay;
    t[SC_PEEK]  = (uint32_t)(uintptr_t)sys_peek;
    t[SC_POKE]  = (uint32_t)(uintptr_t)sys_poke;
    t[SC_PRINT] = (uint32_t)(uintptr_t)sys_print;
    t[SC_PUTS]  = (uint32_t)(uintptr_t)sys_puts;
    t[SC_GETC]  = (uint32_t)(uintptr_t)sys_getc;
}
