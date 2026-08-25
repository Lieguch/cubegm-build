/*
 * stock_ui.h -- 原厂 CubeGM UI 渲染器（前端重写，操作模型复刻原厂）
 *
 * 目的：替换 FrogUI MinUI 文件夹浏览器 → 原厂 5 界面操作模型：
 *   列表页(menu) / 分类页(type) / 历史页 / 收藏页 / 搜索页(search)
 *   + 游戏内设置页(game，L1 呼出，6 项)
 * 底层引擎不变（picoarch + libretro 核），仅 UI 层重写。
 *
 * 按键映射（原厂）：
 *   方向键上下  = 选择条目
 *   方向键左右  = 翻页 / 标签切换（搜索页=软键盘移动）
 *   A           = 确认 / 运行游戏
 *   B           = 返回 / 退格
 *   L1          = 打开游戏内设置
 *   R1          = 进入分类页
 *   SELECT      = 收藏当前游戏
 *   X           = 搜索页清空输入
 *
 * 数据源（原厂格式）：
 *   gamelist/XX.txt  UTF-8 "rom|英文|中文"
 *   covers/XX.idx + 000-008/*.dat  → 480x320 RGB565 封面（索引=列表行序）
 *   recent.lst / favorites.lst     历史/收藏（分号分隔，原厂格式）
 */
#ifndef STOCK_UI_H
#define STOCK_UI_H

#include <stdint.h>
#include <stdbool.h>

#define STOCK_SCREEN_W 1280
#define STOCK_SCREEN_H 720

/* 页面 */
enum {
    STOCK_PAGE_LIST = 0,   /* 列表页 menu.raw */
    STOCK_PAGE_TYPE,       /* 分类页 type.raw */
    STOCK_PAGE_HISTORY,    /* 历史页 */
    STOCK_PAGE_FAV,        /* 收藏页 */
    STOCK_PAGE_SEARCH,     /* 搜索页 search.raw */
    STOCK_PAGE_COUNT
};

/* 分类页 9 平台（原厂 type.raw 顺序） */
#define STOCK_CAT_COUNT 9
extern const char *stock_cat_names[STOCK_CAT_COUNT];       /* 英文缩写 */
extern const char *stock_cat_cn[STOCK_CAT_COUNT];          /* 中文 */

/* 布局（1280x720, 与原厂精灵表对齐） */
#define STOCK_TAG_Y        0
#define STOCK_TAG_H        62
#define STOCK_TAG_W0       146    /* 5 标签各宽 */
#define STOCK_TAG_GAP      8
#define STOCK_TAG_FIRST_X  40

#define STOCK_LIST_X       60     /* 左侧游戏列表 */
#define STOCK_LIST_Y0      110
#define STOCK_LIST_W       620
#define STOCK_LIST_H       56     /* 条目高（可见 10 条） */
#define STOCK_LIST_GAP     6

#define STOCK_COVER_X      760    /* 右侧封面预览区 */
#define STOCK_COVER_Y      130
#define STOCK_COVER_W      480
#define STOCK_COVER_H      320

#define STOCK_LEGEND_Y     650    /* 底部按键提示 */

/* 游戏条目（列表页/搜索/历史/收藏共用） */
typedef struct {
    char rom[160];      /* 文件路径 - rom 名 */
    char en[160];
    char cn[160];
    int  cat;           /* 0-8 */
    bool fav;           /* 是否收藏 */
} StockEntry;

/* API */
void stock_ui_init(void);
void stock_ui_deinit(void);

/* 每帧输入处理（keys 为 cubevol shm 位掩码，bits 见 frogui CV_*） */
void stock_ui_handle_input(uint32_t keys);

/* 渲染到 framebuffer (1280x720 RGB565) */
void stock_ui_render(uint16_t *fb);

/* 回报是否处于"未在分类/未退出"状态（供 frogui 混合） */
bool stock_ui_active(void);

/* 当前选中游戏路径（启动用） */
const char *stock_ui_selected_rom(const char **core_out);

/* 通知：游戏已退出，回到菜单 */
void stock_ui_game_exited(void);

/* 收藏切换 */
void stock_ui_toggle_fav(void);

/* 分类核心映射（原厂分类 → libretro .so） */
extern const char *stock_cat_cores[STOCK_CAT_COUNT];

/* 依据 ROM 路径 (/mnt/sdcard/NNN/xxx) 返回核心 .so 路径，未知返回 NULL */
const char *stock_cat_core_for_rom(const char *rom_path);

#endif /* STOCK_UI_H */