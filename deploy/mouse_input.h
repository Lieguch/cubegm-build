/* mouse_input.h — USB 鼠标输入接口（UI 全面使用） */
#ifndef MOUSE_INPUT_H
#define MOUSE_INPUT_H

#include <stdint.h>

/* 初始化：扫描 /dev/input/event* 找鼠标，返回找到的鼠标数量 */
int mouse_init(void);

/* 每帧轮询：读鼠标事件，更新光标 + 按键状态 */
void mouse_poll(void);

/* 光标坐标 */
int mouse_get_x(void);
int mouse_get_y(void);
int mouse_is_visible(void);

/* 按键状态（btn: 0=左 1=右 2=中） */
int mouse_button_down(int btn);
int mouse_button_pressed(int btn);   /* 边沿：按下瞬间 */
int mouse_button_released(int btn);  /* 边沿：释放瞬间 */

/* 滚轮（累计值，调用后清零） */
int mouse_get_wheel(void);

/* 渲染光标到 framebuffer */
void mouse_render_cursor(uint16_t *framebuffer);

/* 关闭 */
void mouse_deinit(void);

#endif /* MOUSE_INPUT_H */
