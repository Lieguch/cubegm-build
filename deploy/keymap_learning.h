/* keymap_learning.h — 未知手柄键位学习接口 */
#ifndef KEYMAP_LEARNING_H
#define KEYMAP_LEARNING_H

#include <stdint.h>

/* 启动键位学习（检测未知手柄，返回 1=开始学习，0=无未知手柄） */
int keymap_learning_start(void);

/* 每帧轮询（处理鼠标点击 + 手柄按键） */
void keymap_learning_poll(void);

/* 保存映射到 VID_PID 文件 */
int keymap_learning_save(void);

/* 渲染键位学习界面 */
void keymap_learning_render(uint16_t *framebuffer);

/* 是否正在学习 */
int keymap_learning_active(void);

/* 关闭 */
void keymap_learning_stop(void);

#endif /* KEYMAP_LEARNING_H */
