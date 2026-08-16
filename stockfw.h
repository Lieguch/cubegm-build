#ifndef STOCKFW_H
#define STOCKFW_H
/* Minimal stub of the stock-firmware (gs) game-launch interface.
   The open-source libretro frontend (FrogUI) must not depend on the
   closed-source gs library; these symbols are provided as stubs so the
   build links. Real game launching goes through libretro. */
#ifdef __cplusplus
extern "C" {
#endif
extern char *ptr_gs_run_game_file;
extern char *ptr_gs_run_game_name;
void xlog(const char *fmt, ...);
#ifdef __cplusplus
}
#endif
#endif
