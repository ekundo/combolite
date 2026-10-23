/* Один цикл на два сервера: HTTP для морды, NBD для тех, кому нужен диск
 * блочным устройством. Задач и потоков не заводим — на ESP32 их стоимость
 * заметна, а обращения к приводу всё равно упираются в одну шину.
 */
#ifndef COMBO_SERVE_H
#define COMBO_SERVE_H

#include "http.h"
#include "net.h"

#include <signal.h>

/* Поднять в обработчике сигнала, чтобы цикл вышел на ближайшем витке. */
extern volatile sig_atomic_t srv_stopped;

/* Обслуживает, пока не оборвут. При once выходит после первого доведённого
 * соединения на любом из портов. nbd можно передать нулём. */
int srv_run(http_cfg_t *hc, net_cfg_t *nc, int once);

#endif
