#!/usr/bin/env python3
"""integrate_stock_ui.py — 把原厂 UI 渲染器(stock_ui)接入 FrogUI 主循环（v2，实测锚点全部命中）。

改动：
1. include stock_ui.h + stock_dat.h（锚点 #include "settings.h"）
2. Makefile.sf3000 LIBRETRO_SOURCES 加 stock_ui.c + stock_dat.c（锚点 favorites.c）
3. retro_init: 追加 stock_ui_init()
4. helper stub 插在 retro_run 前（g_stock_in_game / launch req / exit reset）
5. retro_run 开头 stock UI 接管（渲染+输入+启动）
6. game exit 恢复 stock UI
7. LDFLAGS 加 -lz（stock_dat uncompress 需要）
"""
import sys, os, shutil

def rep(text, old, new, label):
    if old in text:
        return text.replace(old, new, 1), True
    print(f"  [SKIP] {label}: anchor not found")
    return text, False

HELPER = '''
/* ---- stock UI bridge (integrated by integrate_stock_ui.py) ---- */
int g_stock_in_game = 0;
extern int g_launch_req;                /* 定义于 stock_ui.c */
int stock_ui_launch_req(void) { return g_launch_req; }
void stock_ui_clear_launch(void) { g_launch_req = 0; }
void stock_ui_active_fb(void) { /* stock 模式默认激活；保留 hook */ }
'''

def main():
    if len(sys.argv) < 2:
        print("usage: integrate_stock_ui.py <deploy_dir>", file=sys.stderr)
        sys.exit(1)
    deploy = sys.argv[1]

    for f in ['stock_ui.c', 'stock_ui.h', 'stock_dat.c', 'stock_dat.h']:
        src = os.path.join(deploy, f)
        if os.path.exists(src):
            shutil.copy(src, '.')
            print(f"  [OK] copy {f}")
        else:
            print(f"WARN: {src} missing")

    # Makefile.sf3000
    mk = 'Makefile.sf3000'
    if os.path.exists(mk):
        t = open(mk).read()
        changed = False
        if 'stock_ui.c' not in t:
            for anchor in ['                   favorites.c',
                           '                   background.c']:
                if anchor in t:
                    t = t.replace(anchor, anchor + ' \\\n                   stock_ui.c \\\n                   stock_dat.c', 1)
                    changed = True
                    print("  [OK] Makefile LIBRETRO_SOURCES += stock_ui/stock_dat")
                    break
            if not changed:
                print("  [SKIP] Makefile anchor not found")
        # LDFLAGS -lz
        if '-lz' not in t:
            old_ld = '-lc -lm'
            if old_ld in t:
                t = t.replace(old_ld, '-lc -lm -lz', 1)
                print("  [OK] Makefile LDFLAGS += -lz")
            else:
                print("  [SKIP] Makefile LDFLAGS anchor not found")
        open(mk, 'w').write(t)
    else:
        print("WARN: Makefile.sf3000 missing")

    fg = 'frogui_libretro.c'
    if not os.path.exists(fg):
        print("WARN: frogui_libretro.c missing")
        return
    t = open(fg).read()

    # 1. include（锚点 settings.h —— 上游 frogui_libretro.c 必有）
    if 'stock_ui.h' not in t:
        t, h1 = rep(t, '#include "settings.h"',
                    '#include "settings.h"\n#include "stock_ui.h"\n#include "stock_dat.h"',
                    'include stock_ui.h')
    else:
        print("  [OK] include stock_ui.h (already)")

    # 2. retro_init: cv_init 后 stock_ui_init
    if 'stock_ui_init();' not in t:
        t, h2 = rep(t, '    cv_init();',
                    '    cv_init();\n    stock_ui_init();\n    stock_ui_active_fb();',
                    'retro_init stock_ui_init')
    else:
        print("  [OK] stock_ui_init (already)")

    # 3. helper stub 插到 include 区之后（全局区，供全文件使用）
    if 'int stock_ui_launch_req(void)' not in t:
        anchor_inc = '#include "stock_ui.h"\n#include "stock_dat.h"'
        if anchor_inc in t:
            t = t.replace(anchor_inc, anchor_inc + HELPER, 1)
            print("  [OK] helper stubs inserted after includes")
        else:
            print("  [SKIP] helper: include anchor not found")
    else:
        print("  [OK] helper stubs (already)")

    # 4. retro_run 接管（anchor 含 handle_input 首行，避免重复）
    anchor = 'void retro_run(void) {\n    handle_input();'
    repl = '''void retro_run(void) {
    /* 原厂 UI 模式：stock UI 全权接管（列表/分类/搜索/收藏/历史渲染+输入） */
    if (stock_ui_active() && !g_stock_in_game) {
        stock_ui_handle_input(cv_read());
        stock_ui_render(framebuffer);
        if (video_cb)
            video_cb(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * sizeof(uint16_t));
        if (stock_ui_launch_req()) {
            const char *rom = stock_ui_selected_rom(NULL);
            const char *core = NULL;
            if (rom) {
                g_stock_in_game = 1;
                core = stock_cat_core_for_rom(rom);
                if (core)
                    request_game_launch(core, rom);
            }
            stock_ui_clear_launch();
        }
        return;
    }
    handle_input();'''
    if anchor in t and 'stock_ui_handle_input(cv_read())' not in t:
        t = t.replace(anchor, repl, 1)
        print("  [OK] retro_run stock UI takeover")
    else:
        print("  [SKIP] retro_run anchor missing/already integrated")

    # 5. game exit 恢复
    t, h6 = rep(t, 'dbg("game finished, returning to frogui");',
                'dbg("game finished, returning to frogui");\n    g_stock_in_game = 0;\n    stock_ui_game_exited();',
                'game exit reset')

    open(fg, 'w').write(t)
    print("FrogUI stock UI integration done (v2).")

if __name__ == '__main__':
    main()