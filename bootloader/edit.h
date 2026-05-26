/*
 * NeOS modal text editor (vim subset).
 *
 * Runs on the HDMI framebuffer using the 5x7 font in fb.c, fits ~26 cols
 * x 13 visible lines of text plus a status row at the top and a command
 * row at the bottom.
 *
 * Modes:
 *   NORMAL  : hjkl/arrows move, i/a/o/O enter INSERT, x/dd delete,
 *             ":" enters COMMAND
 *   INSERT  : typing inserts, ESC -> NORMAL
 *   COMMAND : ":" prefix; commands {w, run, q, clear, help}
 *
 * `:w`     copies the buffer into the cc command line and compiles it
 *          (does NOT run; just compiles + prints instr count).
 * `:run`   compiles + jumps to the compiled code immediately.
 * `:q`     exits the editor.
 * `:clear` empties the buffer.
 */

#ifndef NEOS_EDIT_H
#define NEOS_EDIT_H

void edit_run(void);

#endif
