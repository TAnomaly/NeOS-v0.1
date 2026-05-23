#ifndef INTERP_H
#define INTERP_H
#include <stdint.h>

/* Caller supplies a single-line input (NUL-terminated, no trailing \r\n).
   Tries to interpret it as a C-like statement. Returns 1 if handled
   (even on parse/eval error — error message printed via puts_both),
   0 if line is not a recognized interpreter statement so the caller can
   fall through to the legacy command dispatcher. */
int interp_try(const char *line);

#endif
