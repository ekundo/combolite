/* USB-мышь в розетке платы -> ПЛИС -> порт D4h машины (fpga/hdl/mouse.v).
 * Хост USB ESP32-S3, mouse_esp.c. */
#ifndef COMBO_MOUSE_H
#define COMBO_MOUSE_H

/* Поднять хост USB и ждать мышь. Звать один раз, после link_open и
 * board_init: отчёты уходят в ПЛИС сразу из задачи USB. */
void mouse_start(void);

#endif
