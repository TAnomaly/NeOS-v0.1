#ifndef CC_H
#define CC_H
#include <stdint.h>

/* Compile source `src` into RV32IM machine code starting at code_base.
   Returns number of 32-bit instructions emitted, or -1 on error.
   On error, an error message is printed via puts_both before returning. */
int cc_compile(const char *src, uint32_t *code_base, int max_words);

/* Jump into compiled code at code_base. Returns when compiled code RETs. */
void cc_run(uint32_t *code_base);

#endif
