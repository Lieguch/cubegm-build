#!/usr/bin/env python3
"""integrate_frogui_modules.py — 把鼠标/键位学习/背景图模块完整集成进 FrogUI

用 Python 精确替换（避免 sed 转义地狱），每处替换独立防卫（失败仅 WARN，不崩溃）：
1. 复制新模块源文件到 FrogUI 目录
2. 修改 Makefile.sf3000 的 LIBRETRO_SOURCES
3. 修改 frogui_libretro.c：
   a. 加 include
   b. retro_init: mouse_init + background_init + 键位学习自动触发（无 keymap.txt 时）
   c. retro_run: mouse_poll + keymap_learning_poll 跑在 handle_input 之前
   d. handle_input: 鼠标左键→A(确认)、右键→B(返回)、滚轮→上/下 合入按钮状态
   e. retro_run 渲染末尾: keymap_learning_render + mouse_render_cursor

用法：python3 integrate_frogui_modules.py <deploy_dir>
（在 FrogUI 目录内执行，deploy_dir 指向新模块源文件所在目录）

输出：打印每处替换是否命中（applied/skip/missing），便于 CI 日志核对。
"""

import sys
import os
import shutil

REPORT = []


def rep(text, old, new):
    """replace first occurrence; return (text, hit)."""
    if old in text:
        return text.replace(old, new, 1), True
    return text, False


def main():
    if len(sys.argv) < 2:
        print("usage: integrate_frogui_modules.py <deploy_dir>", file=sys.stderr)
        sys.exit(1)
    deploy_dir = sys.argv[1]

    # 1. 复制新模块（含 UTF-8 中文版 font.c/font.h，覆盖上游单字节 ASCII 版本）
    files = ['mouse_input.c', 'mouse_input.h',
             'keymap_learning.c', 'keymap_learning.h',
             'background.c', 'background.h',
             'font.c', 'font.h']
    for f in files:
        src = os.path.join(deploy_dir, f)
        if os.path.exists(src):
            shutil.copy(src, '.')
        else:
            print(f"WARN: {src} missing", file=sys.stderr)

    # 2. 修改 Makefile.sf3000 的 LIBRETRO_SOURCES
    mk_path = 'Makefile.sf3000'
    if os.path.exists(mk_path):
        mk = open(mk_path).read()
        if 'mouse_input.c' not in mk:
            lines = mk.split('\n')
            in_libretro = False
            new_lines = []
            for line in lines:
                new_lines.append(line)
                if 'LIBRETRO_SOURCES' in line:
                    in_libretro = True
                    continue
                if in_libretro and 'favorites.c' in line and not line.rstrip().endswith('\\'):
                    new_lines[-1] = line + ' \\'
                    new_lines.append('                   mouse_input.c \\')
                    new_lines.append('                   keymap_learning.c \\')
                    new_lines.append('                   background.c')
                    in_libretro = False
            open(mk_path, 'w').write('\n'.join(new_lines))
            REPORT.append(("Makefile.sf3000 LIBRETRO_SOURCES", True))
        else:
            REPORT.append(("Makefile.sf3000 LIBRETRO_SOURCES", "skip"))
    else:
        REPORT.append(("Makefile.sf3000", "missing"))

    # 3. 修改 frogui_libretro.c
    fg_path = 'frogui_libretro.c'
    if not os.path.exists(fg_path):
        REPORT.append(("frogui_libretro.c", "missing"))
        print("FrogUI modules integration report:")
        for name, r in REPORT:
            print(f"  {name}: {r}")
        sys.exit(0)

    fg = open(fg_path).read()

    # 3a. include
    if 'mouse_input.h' not in fg:
        fg, hit = rep(fg, '#include "settings.h"',
                      '#include "settings.h"\n#include "mouse_input.h"\n#include "keymap_learning.h"\n#include "background.h"')
        REPORT.append(("include headers", hit))
    else:
        REPORT.append(("include headers", "skip"))

    # 3b. retro_init：mouse_init + background_init + 键位学习自动触发
    if 'mouse_init();' not in fg:
        fg, hit = rep(fg, '    cv_init();',
                      '    cv_init();\n    mouse_init();\n    background_init();\n    /* 首次运行(无 keymap.txt)自动进入键位学习 */\n    if (access("/mnt/sdcard/cubegm/keymap.txt", F_OK) != 0) {\n        keymap_learning_start();\n    }')
        REPORT.append(("retro_init mouse/background/keymap", hit))
    else:
        REPORT.append(("retro_init mouse/background/keymap", "skip"))

    # 3c. retro_run 开头：mouse_poll + keymap_learning_poll 在 handle_input 之前
    fg, hit = rep(fg, '    handle_input();',
                  '    mouse_poll();\n    keymap_learning_poll();\n    handle_input();')
    REPORT.append(("retro_run poll-before-input", hit))

    # 3d. handle_input：鼠标左键→A(确认)、右键→B(返回)
    fg, h1 = rep(fg, 'bool a  = cv_btn(keys, CV_A);',
                 'bool a  = cv_btn(keys, CV_A)  || mouse_button_down(0);')
    fg, h2 = rep(fg, 'bool b  = cv_btn(keys, CV_B);',
                 'bool b  = cv_btn(keys, CV_B)  || mouse_button_down(1);')
    REPORT.append(("handle_input mouse A/B", h1 and h2))

    # 3e. handle_input：滚轮→上/下导航
    fg, h3 = rep(fg, 'bool up = cv_btn(keys, CV_UP);',
                 'bool up = cv_btn(keys, CV_UP)  || (mouse_get_wheel() > 0);')
    fg, h4 = rep(fg, 'bool dn = cv_btn(keys, CV_DOWN);',
                 'bool dn = cv_btn(keys, CV_DOWN) || (mouse_get_wheel() < 0);')
    REPORT.append(("handle_input wheel up/down", h3 and h4))

    # 3f. 渲染：主菜单纯色背景 → 原厂背景图
    fg, h5 = rep(fg, '        render_clear_screen(framebuffer);',
                 '        background_render(framebuffer);')
    REPORT.append(("render background_render", h5))

    # 3g. 渲染末尾：键位学习界面 + 鼠标光标（在 video_cb 前）
    if 'mouse_render_cursor(framebuffer);' not in fg:
        fg, hit = rep(fg, '    if (video_cb)',
                      '    keymap_learning_render(framebuffer);\n    mouse_render_cursor(framebuffer);\n    if (video_cb)')
        REPORT.append(("render keymap/cursor", hit))
    else:
        REPORT.append(("render keymap/cursor", "skip"))

    open(fg_path, 'w').write(fg)
    print("FrogUI modules integration report:")
    for name, r in REPORT:
        print(f"  {name}: {r}")


if __name__ == '__main__':
    main()