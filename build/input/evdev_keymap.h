/* Auto-generated from joystick.zip by tools/gen_evdev_keymap.py -- DO NOT EDIT BY HAND */
#ifndef CUBEGM_EVDEV_KEYMAP_H
#define CUBEGM_EVDEV_KEYMAP_H

/* 13 canonical actions (shared by P1/P2) */
enum cube_action {
  ACT_NONE = -1,
  ACT_SELECT = 0,
  ACT_START = 1,
  ACT_UP = 2,
  ACT_DOWN = 3,
  ACT_LEFT = 4,
  ACT_RIGHT = 5,
  ACT_A = 6,
  ACT_B = 7,
  ACT_TL1 = 8,
  ACT_X = 9,
  ACT_Y = 10,
  ACT_TR1 = 11,
  ACT_RESET = 12
  ACT_COUNT = 13
};

/* 26-slot action table from joystick.zip '0000_0000': P1[0..12] then P2[13..25] */
static const char *ACTION_NAMES[26] = {
  "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "TL1", "X", "Y", "TR1", "RESET", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "TL1", "X", "Y", "TR1", "RESET"
};

/* ui.cfg: 2 rows (P1,P2) x 27 cols. col0 unused; cols 1..26 map to ACTION_NAMES[0..25] */
#define UI_CFG_COLS 27
static const int UI_CFG[2][UI_CFG_COLS] = {
  { 0, 3, 2, 32, 4, 41, 33, 36, 1, 31, 40, 35, 34, 38, 37, 39, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
  { 0, 18, 17, 43, 19, 52, 44, 47, 16, 42, 51, 46, 45, 49, 48, 50, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 },
};

/* reverse map: evdev keycode -> action index (0..25); -1 = unbound */
static inline int evdev_to_action(int player, int code) {
  if (player < 0 || player > 1) return -1;
  for (int col = 1; col < UI_CFG_COLS; col++)
    if (UI_CFG[player][col] == code) return col - 1;
  return -1;
}

/* USB gamepad profiles (VID_PID[_REV]); 17 button slots + [hat, axisX, axisY] */
struct usb_profile {
  const char *name; int vid, pid, rev;
  const char *btn[17]; int hat, axis_x, axis_y;
};
static const struct usb_profile USB_PROFILES[] = {
  { "0810_0001_0100", 0x0810, 0x0001, 256, { "A", "B", "TL1", "X", "Y", "TR1", "TL1", "TR1", "TL2", "TR2", "SELECT", "START", "0", "0", "0", "0", "0" }, 1, 0, 1 },
  { "20bc_5500", 0x20BC, 0x5500, -1, { "A", "B", "0", "X", "Y", "0", "TL1", "TR1", "TL2", "TR2", "SELECT", "START", "RESET", "0", "0", "0", "0" }, 1, 6, 7 },
  { "0810_0001_0110", 0x0810, 0x0001, 272, { "Y", "B", "A", "X", "TL1", "TR1", "TL2", "TR2", "SELECT", "START", "0", "0", "0", "0", "0", "0", "0" }, 1, 4, 5 },
  { "2563_0555", 0x2563, 0x0555, -1, { "Y", "B", "A", "X", "TL1", "TR1", "TL2", "TR2", "SELECT", "START", "0", "0", "0", "0", "0", "0", "0" }, 1, 4, 5 },
};
#define USB_PROFILE_COUNT 4

#endif /* CUBEGM_EVDEV_KEYMAP_H */
