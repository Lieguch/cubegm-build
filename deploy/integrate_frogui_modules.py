#!/usr/bin/env python3
"""integrate_frogui_modules.py — 把鼠标/键位学习/背景图模块集成进 FrogUI

用 Python 精确修改（避免 sed 转义地狱）：
1. 复制新模块源文件到 FrogUI 目录
2. 修改 Makefile.sf3000 的 LIBRETRO_SOURCES
3. 修改 frogui_libretro.c（加 include + 调用点）

用法：python3 integrate_frogui_modules.py <deploy_dir>
（在 FrogUI 目录内执行，deploy_dir 指向新模块源文件所在目录）
"""

import sys
import os
import shutil

def main():
    if len(sys.argv) < 2:
        print("usage: integrate_frogui_modules.py <deploy_dir>", file=sys.stderr)
        sys.exit(1)
    deploy_dir = sys.argv[1]

    # 1. 复制新模块
    files = ['mouse_input.c', 'mouse_input.h',
             'keymap_learning.c', 'keymap_learning.h',
             'background.c', 'background.h']
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
            # 在 LIBRETRO_SOURCES 的 favorites.c（无续行符的最后一行）后加
            # 找到 LIBRETRO_SOURCES 段的 favorites.c（无 \ 结尾）
            lines = mk.split('\n')
            in_libretro = False
            new_lines = []
            for line in lines:
                new_lines.append(line)
                if 'LIBRETRO_SOURCES' in line:
                    in_libretro = True
                    continue
                if in_libretro and 'favorites.c' in line and not line.rstrip().endswith('\\'):
                    # 这是 LIBRETRO_SOURCES 的最后一行，加续行符 + 新模块
                    new_lines[-1] = line + ' \\'
                    new_lines.append('                   mouse_input.c \\')
                    new_lines.append('                   keymap_learning.c \\')
                    new_lines.append('                   background.c')
                    in_libretro = False
            open(mk_path, 'w').write('\n'.join(new_lines))
            print("Makefile.sf3000 updated")
        else:
            print("Makefile.sf3000 already has mouse_input.c, skip")

    # 3. 修改 frogui_libretro.c
    fg_path = 'frogui_libretro.c'
    if os.path.exists(fg_path):
        fg = open(fg_path).read()
        changed = False

        # 3a. 加 include
        if 'mouse_input.h' not in fg:
            fg = fg.replace('#include "settings.h"',
                            '#include "settings.h"\n#include "mouse_input.h"\n#include "keymap_learning.h"\n#include "background.h"')
            changed = True

        # 3b. retro_init 加 mouse_init + background_init
        if 'mouse_init();' not in fg:
            fg = fg.replace('    cv_init();',
                            '    cv_init();\n    mouse_init();\n    background_init();')
            changed = True

        # 3c. retro_run 的 render_clear_screen 改为 background_render
        if 'background_render(framebuffer);' not in fg:
            # 只改 retro_run 里的（8空格缩进的 render_clear_screen）
            fg = fg.replace('        render_clear_screen(framebuffer);',
                            '        background_render(framebuffer);')
            changed = True

        # 3d. retro_run 末尾加 mouse_poll + 光标
        if 'mouse_render_cursor(framebuffer);' not in fg:
            fg = fg.replace('    if (video_cb)',
                            '    mouse_poll();\n    mouse_render_cursor(framebuffer);\n    if (video_cb)')
            changed = True

        if changed:
            open(fg_path, 'w').write(fg)
            print("frogui_libretro.c updated")

    print("FrogUI modules integrated")

if __name__ == '__main__':
    main()
