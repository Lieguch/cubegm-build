#ifndef FONT_H
#define FONT_H

#include <stdint.h>

// Initialize font system
void font_init(void);

// Load font from settings (call when font setting changes)
void font_load_from_settings(const char *font_name);

// Draw a single character at position (x, y) with given color
// 注意：codepoint 为 Unicode 码点（非单字节 char），支持 UTF-8 解码后的中文码点
void font_draw_char(uint16_t *framebuffer, int screen_width, int screen_height,
                   int x, int y, int codepoint, uint16_t color);

// Draw a text string at position (x, y) with given color
// text 为 UTF-8 编码字符串，内部按码点解码渲染（支持中文）
void font_draw_text(uint16_t *framebuffer, int screen_width, int screen_height,
                   int x, int y, const char *text, uint16_t color);

// Measure text width in pixels
int font_measure_text(const char *text);

// Get font character width/height — scale with UI_SCALE
#ifndef UI_SCALE
#define UI_SCALE 100
#endif
#define FONT_CHAR_WIDTH    (18 * UI_SCALE / 100)
#define FONT_CHAR_HEIGHT   (20 * UI_SCALE / 100)
#define FONT_CHAR_SPACING  (13 * UI_SCALE / 100)

#endif // FONT_H