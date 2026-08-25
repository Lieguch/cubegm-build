#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "font.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static stbtt_fontinfo font_info;
static unsigned char *font_buffer = NULL;
static float font_scale;
static int font_loaded = 0;

#ifndef UI_SCALE
#define UI_SCALE 100
#endif
#define FONT_SIZE (20.0f * UI_SCALE / 100.0f)

/* UTF-8 → Unicode codepoint 解码。返回消耗字节数（0=串结束），*cp 为码点。
 * 支持 1~4 字节 UTF-8（覆盖 ASCII + CJK 等），使中文字体可正常渲染。 */
static int utf8_next(const char *s, int *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (!u[0]) return 0;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    else if ((u[0] & 0xE0) == 0xC0) {
        *cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F); return 2;
    } else if ((u[0] & 0xF0) == 0xE0) {
        *cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F); return 3;
    } else if ((u[0] & 0xF8) == 0xF0) {
        *cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12)
            | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F); return 4;
    }
    *cp = u[0]; return 1;  /* 非法字节：按单字节回退 */
}

// Internal function to load a font file
static int load_font_file(const char *font_filename) {
    // Free previous font if loaded
    if (font_buffer) {
        free(font_buffer);
        font_buffer = NULL;
        font_loaded = 0;
    }

    // Build search paths for the font (SF3000 paths first；新增原厂 font.ttf 路径)
    char font_paths[5][256];
    snprintf(font_paths[0], sizeof(font_paths[0]), "/mnt/sdcard/cubegm/fonts/%s", font_filename);
    snprintf(font_paths[1], sizeof(font_paths[1]), "/mnt/sdcard/cubegm/%s", font_filename);
    snprintf(font_paths[2], sizeof(font_paths[2]), "/mnt/sdcard/frogui/fonts/%s", font_filename);
    snprintf(font_paths[3], sizeof(font_paths[3]), "/mnt/sda1/frogui/fonts/%s", font_filename);
    snprintf(font_paths[4], sizeof(font_paths[4]), "fonts/%s", font_filename);

    FILE *fp = NULL;
    for (int i = 0; i < 5; i++) {
        fp = fopen(font_paths[i], "rb");
        if (fp) break;
    }

    if (!fp) {
        return 0;
    }

    // Get file size
    fseek(fp, 0, SEEK_END);
    long font_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // Allocate buffer and read font
    font_buffer = (unsigned char*)malloc(font_size);
    if (!font_buffer) {
        fclose(fp);
        return 0;
    }

    fread(font_buffer, 1, font_size, fp);
    fclose(fp);

    // Initialize font
    if (!stbtt_InitFont(&font_info, font_buffer, stbtt_GetFontOffsetForIndex(font_buffer, 0))) {
        free(font_buffer);
        font_buffer = NULL;
        return 0;
    }

    // Calculate scale for desired pixel height
    font_scale = stbtt_ScaleForPixelHeight(&font_info, FONT_SIZE);
    font_loaded = 1;
    return 1;
}

void font_load_from_settings(const char *font_name) {
    const char *font_filename = NULL;
    float custom_size = FONT_SIZE;

    // Map font names to font files — always render at FONT_SIZE (scaled by UI_SCALE)
    if (strcmp(font_name, "Monogram") == 0) {
        font_filename = "monogram.ttf";
    } else if (strcmp(font_name, "Chinese") == 0 || strcmp(font_name, "CJK") == 0) {
        font_filename = "font.ttf";   /* 原厂中文字体 /mnt/sdcard/cubegm/font.ttf */
    } else {
        font_filename = "GamePocket-Regular-ZeroKern.ttf";
    }
    custom_size = FONT_SIZE;  // use compile-time size, not hardcoded per-font px

    load_font_file(font_filename);

    if (font_loaded) {
        font_scale = stbtt_ScaleForPixelHeight(&font_info, custom_size);
    }
}

void font_init(void) {
    // Load default font initially
    font_load_from_settings("GamePocket");
}

void font_draw_char(uint16_t *framebuffer, int screen_width, int screen_height,
                   int x, int y, int codepoint, uint16_t color) {
    if (!font_loaded || !framebuffer) return;

    // Convert ASCII lowercase to uppercase (CJK codepoints unchanged)
    if (codepoint >= 'a' && codepoint <= 'z') {
        codepoint = codepoint - 'a' + 'A';
    }

    // Get glyph index
    int glyph_index = stbtt_FindGlyphIndex(&font_info, codepoint);
    if (glyph_index == 0) {
        /* 当前字体无此码点（如 Latin 字体遇中文）→ 回退到中文字体 font.ttf 试一次 */
        if (codepoint >= 0x80) {
            static int cjk_fallback_tried = 0;
            if (!cjk_fallback_tried) {
                cjk_fallback_tried = 1;
                static unsigned char *cjk_buf = NULL;
                static stbtt_fontinfo cjk_info;
                FILE *f = fopen("/mnt/sdcard/cubegm/font.ttf", "rb");
                if (f) {
                    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                    cjk_buf = (unsigned char*)malloc(sz);
                    if (cjk_buf) {
                        fread(cjk_buf, 1, sz, f);
                        if (stbtt_InitFont(&cjk_info, cjk_buf, stbtt_GetFontOffsetForIndex(cjk_buf, 0))) {
                            glyph_index = stbtt_FindGlyphIndex(&cjk_info, codepoint);
                            /* 复用 font_scale，用 cjk_info 渲染该码点 */
                            if (glyph_index != 0) {
                                int w2, h2, xo2, yo2;
                                unsigned char *b2 = stbtt_GetGlyphBitmap(&cjk_info, 0, font_scale,
                                                                         glyph_index, &w2, &h2, &xo2, &yo2);
                                if (b2) {
                                    int asc2, dsc2, lg2;
                                    stbtt_GetFontVMetrics(&cjk_info, &asc2, &dsc2, &lg2);
                                    int bl2 = (int)(asc2 * font_scale);
                                    for (int r = 0; r < h2; r++)
                                        for (int c = 0; c < w2; c++)
                                            if (b2[r * w2 + c] > 127) {
                                                int px = x + xo2 + c, py = y + bl2 + yo2 + r;
                                                if (px >= 0 && px < screen_width && py >= 0 && py < screen_height)
                                                    framebuffer[py * screen_width + px] = color;
                                            }
                                    stbtt_FreeBitmap(b2, NULL);
                                    return;
                                }
                            }
                        }
                    }
                    fclose(f);
                }
            }
        }
        return; // Glyph not found
    }

    // Get glyph bitmap
    int width, height, xoff, yoff;
    unsigned char *bitmap = stbtt_GetGlyphBitmap(&font_info, 0, font_scale,
                                                  glyph_index, &width, &height, &xoff, &yoff);

    if (!bitmap) return;

    // Get vertical metrics for proper baseline alignment
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
    int baseline = (int)(ascent * font_scale);

    // Draw the glyph
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            unsigned char alpha = bitmap[row * width + col];
            if (alpha > 0) {
                int px = x + xoff + col;
                int py = y + baseline + yoff + row;

                if (px >= 0 && px < screen_width && py >= 0 && py < screen_height) {
                    // Simple alpha blending
                    if (alpha > 127) {
                        framebuffer[py * screen_width + px] = color;
                    }
                }
            }
        }
    }

    stbtt_FreeBitmap(bitmap, NULL);
}

void font_draw_text(uint16_t *framebuffer, int screen_width, int screen_height,
                   int x, int y, const char *text, uint16_t color) {
    if (!font_loaded || !framebuffer || !text) return;

    int start_x = x;
    int prev_codepoint = 0;

    while (*text) {
        if (*text == '\n') {
            y += FONT_SIZE + 4;  // Line spacing
            x = start_x;
            text++;
            prev_codepoint = 0;
            continue;
        }

        /* UTF-8 解码：一次取一个完整码点（ASCII/CJK 通用） */
        int codepoint = 0;
        int n = utf8_next(text, &codepoint);

        // Get glyph index
        int glyph_index = stbtt_FindGlyphIndex(&font_info, codepoint);

        if (glyph_index != 0) {
            // Get advance width and left side bearing
            int advance_width, left_side_bearing;
            stbtt_GetGlyphHMetrics(&font_info, glyph_index, &advance_width, &left_side_bearing);

            // Apply kerning if we have a previous character
            if (prev_codepoint != 0) {
                int kern = stbtt_GetGlyphKernAdvance(&font_info, prev_codepoint, glyph_index);
                x += (int)(kern * font_scale);
            }

            // Draw the character
            font_draw_char(framebuffer, screen_width, screen_height, x, y, codepoint, color);

            // Advance cursor
            x += (int)(advance_width * font_scale);
            prev_codepoint = glyph_index;
        } else {
            // Space or unknown character（含需回退到中文字体的码点）
            if (codepoint >= 0x80) {
                font_draw_char(framebuffer, screen_width, screen_height, x, y, codepoint, color);
                x += (int)(FONT_CHAR_WIDTH * 0.9f);
            } else {
                x += FONT_CHAR_SPACING;
            }
            prev_codepoint = 0;
        }

        text += n;
    }
}

int font_measure_text(const char *text) {
    if (!text || !font_loaded) return 0;

    int width = 0;
    int prev_codepoint = 0;

    while (*text) {
        // Skip newlines
        if (*text == '\n') {
            text++;
            prev_codepoint = 0;
            continue;
        }

        int codepoint = 0;
        int n = utf8_next(text, &codepoint);

        // Get glyph index
        int glyph_index = stbtt_FindGlyphIndex(&font_info, codepoint);

        if (glyph_index != 0) {
            // Get advance width
            int advance_width, left_side_bearing;
            stbtt_GetGlyphHMetrics(&font_info, glyph_index, &advance_width, &left_side_bearing);

            // Apply kerning if we have a previous character
            if (prev_codepoint != 0) {
                int kern = stbtt_GetGlyphKernAdvance(&font_info, prev_codepoint, glyph_index);
                width += (int)(kern * font_scale);
            }

            // Add character width
            width += (int)(advance_width * font_scale);
            prev_codepoint = glyph_index;
        } else {
            // Space or unknown character
            width += FONT_CHAR_SPACING;
            prev_codepoint = 0;
        }

        text += n;
    }

    return width;
}