/* background.h — 原厂背景图加载接口 */
#ifndef BACKGROUND_H
#define BACKGROUND_H

#include <stdint.h>

/* 背景图类型（对应 5 个界面） */
typedef enum {
    BG_MENU = 0,
    BG_SEARCH,
    BG_SETTING,
    BG_TYPE,
    BG_GAME,
    BG_COUNT
} BackgroundType;

/* 初始化：加载默认背景（菜单） */
void background_init(void);

/* 加载指定背景 */
int background_load(BackgroundType type);

/* 切换背景（进入搜索/设置/分类/游戏界面时调用） */
void background_switch(BackgroundType type);

/* 渲染背景到 framebuffer（替代 render_clear_screen 的纯色填充） */
void background_render(uint16_t *framebuffer);

/* 释放 */
void background_deinit(void);

#endif /* BACKGROUND_H */
