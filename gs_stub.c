/* Stub implementation of the stock-firmware gs game-launch interface.
   See stockfw.h. Buffers are pre-allocated so sprintf into them will not
   crash; the launch path itself is a no-op in the open-source build. */
#include <stdarg.h>
#include <stdio.h>
#include "stockfw.h"

static char _gs_run_game_file_buf[8192];
static char _gs_run_game_name_buf[8192];

char *ptr_gs_run_game_file = _gs_run_game_file_buf;
char *ptr_gs_run_game_name = _gs_run_game_name_buf;

void xlog(const char *fmt, ...) {
    (void)fmt; /* no-op stub: logging is non-essential for the open-source build */
}
