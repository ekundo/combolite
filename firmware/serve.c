#include "serve.h"
#include "ata.h"
#include "board.h"
#include "fpga.h"

#include <errno.h>
/* sys/poll.h, а не poll.h: в ESP-IDF есть только первый (newlib кладёт его в
   platform_include/sys). */
#include <sys/poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/time.h>

/* Сколько соединений держим разом. Восьми хватает: браузер открывает до шести,
 * плюс клиент NBD. Больше на ESP32 незачем — упираемся всё равно в одну шину. */
#define MAX_CONNS   8
#define HTTP_IDLE_S 10          /* столько держим открытым простаивающий HTTP */
#define NBD_CONNS    2

volatile sig_atomic_t srv_stopped;

struct conn {
    int    fd;
    int    is_nbd;
    time_t last;                /* когда в нём последний раз что-то было */
};

/* Часы монотонные: настенные на ESP32 до синхронизации могут скакнуть, и
 * простаивающие соединения закрылись бы разом или не закрылись вовсе. */
static time_t now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

int srv_run(http_cfg_t *hc, net_cfg_t *nc, int once)
{
    struct pollfd p[2 + MAX_CONNS];
    struct conn c[MAX_CONNS];
    int drop[MAX_CONNS];
    int lfd[2], lkind[2];       /* 0 — HTTP, 1 — NBD */
    int nl = 0, nc_used = 0, done = 0, i;

    if (hc) {
        int fd = http_listen(hc->port);
        if (fd < 0) { fprintf(stderr, "port %u is busy\n", (unsigned)hc->port); return -1; }
        lfd[nl] = fd; lkind[nl] = 0; nl++;
        printf("HTTP listening on port %u, static files from %s\n", (unsigned)hc->port, hc->root);
    }
    if (nc) {
        int fd = nbd_listen(nc->port);
        if (fd < 0) { fprintf(stderr, "port %u is busy\n", (unsigned)nc->port); return -1; }
        lfd[nl] = fd; lkind[nl] = 1; nl++;
        printf("NBD listening on port %u\n", (unsigned)nc->port);
    }
    if (!nl) return -1;
    fflush(stdout);

    while (!srv_stopped) {
        int nfds = 0, r;
        time_t t;

        for (i = 0; i < nl; i++)      { p[nfds].fd = lfd[i];  p[nfds].events = POLLIN; nfds++; }
        for (i = 0; i < nc_used; i++) { p[nfds].fd = c[i].fd; p[nfds].events = POLLIN; nfds++; }

        /* Пока привод поднимается, оборачиваемся чаще: шаг сброса делается
         * раз в виток, и на секундном опросе готовность замечалась бы с
         * секундным опозданием. */
        r = poll(p, (unsigned)nfds,
                 ata_drive_state() == ATA_DRIVE_RESET ? 200 : 1000);
        if (srv_stopped) break;
        if (r < 0) { if (errno == EINTR) continue; break; }
        t = now_s();

        /* Один шаг фоновых дел привода. Дёшев: захват шины при молчащей
         * машине плюс одно чтение регистра. */
        ata_tick();
        /* И один шаг узлов ПЛИС: перезалитой ПЛИС вернуть настройки. */
        board_tick();
        fpga_tick((uint32_t)t);     /* брошенную загрузку образа ПЛИС -- выбросить */

        /* Сперва уже принятые: по ОДНОМУ запросу за виток. Так ни долгий сеанс
         * NBD, ни соединение браузера не запирают остальных.
         *
         * УПЛОТНЯТЬ МАССИВ ЗДЕСЬ НЕЛЬЗЯ. События лежат в p[] в прежнем порядке,
         * и стоит переставить c[], как чужой revents достанется не тому
         * соединению. Поэтому сначала помечаем, потом уплотняем. */
        for (i = 0; i < nc_used; i++) {
            int ev = p[nl + i].revents;

            drop[i] = 0;
            if (ev & (POLLHUP | POLLERR)) drop[i] = 1;
            else if (ev & POLLIN) {
                c[i].last = t;
                drop[i] = (c[i].is_nbd ? nbd_request(c[i].fd, nc)
                                       : http_request(c[i].fd, hc)) < 0;
            }
            else if (!c[i].is_nbd && t - c[i].last > HTTP_IDLE_S) {
                /* Простаивающее соединение браузера занимает место. У NBD так
                 * нельзя: смонтированный диск может молчать часами. */
                drop[i] = 1;
            }
        }
        {   int w = 0;
            for (i = 0; i < nc_used; i++) {
                if (drop[i]) { close(c[i].fd); done++; }
                else c[w++] = c[i];
            }
            nc_used = w;
        }

        for (i = 0; i < nl; i++) {
            struct timeval tv = { 5, 0 };
            int fd, nbd_open = 0, k;

            struct sockaddr_in peer;
            socklen_t plen = sizeof peer;

            if (!(p[i].revents & POLLIN)) continue;
            fd = accept(lfd[i], (struct sockaddr *)&peer, &plen);
            if (fd < 0) continue;

            /* Отключить алгоритм Нейгла НАДО ЗДЕСЬ, на принятом сокете: живой
             * путь идёт через accept и http_request. Ответ уходит двумя
             * посылками, заголовок и тело, и Нейгл вправе придержать вторую до
             * подтверждения первой.
             *
             * ВЫИГРЫШ ИЗМЕРИТЬ НЕ УДАЛОСЬ, и придумывать его не будем. На плате
             * разброс от качества радио (десятки миллисекунд на запрос, см.
             * wifi_rssi) больше самого эффекта. Так что это правка по
             * рассуждению, а не по замеру, — и знать об этом полезнее, чем
             * иметь в комментарии красивое число.
             *
             * Ставим обоим видам соединений: NBD выставляет своё в net.c, но
             * лишний раз не помешает, а забыть здесь — помешает. */
            {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            }

            /* Список разрешённых адресов — ПЕРИМЕТР, а не авторизация: адрес
             * берётся из самого соединения, а не из заголовка X-Forwarded-For,
             * который ставит кто угодно. Проверка секрета остаётся и при
             * непустом списке, см. allow.h. */
            {
                const allow_t *al = lkind[i] == 0 ? hc->allow
                                  : (nc ? nc->allow : 0);
                if (al && !allow_ok(al, ntohl(peer.sin_addr.s_addr))) {
                    close(fd);
                    continue;
                }
            }

            /* Таймаут приёма — страховка от полузамолчавшего клиента: запрос
             * читается целиком, и без него оборванный на середине заголовок
             * застопорил бы весь цикл. */
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

            for (k = 0; k < nc_used; k++) if (c[k].is_nbd) nbd_open++;

            /* Мест нет — вытесняем самое давнее простаивающее соединение
             * браузера. Ждать, пока оно протухнет по времени, нельзя: браузер
             * открывает соединения впрок пачкой, и следующий клиент получал бы
             * отказ на все десять секунд. Соединения NBD не трогаем — там
             * молчание в порядке вещей, диск может быть смонтирован без дела. */
            if (nc_used >= MAX_CONNS) {
                int old = -1;
                for (k = 0; k < nc_used; k++)
                    if (!c[k].is_nbd && (old < 0 || c[k].last < c[old].last))
                        old = k;
                if (old < 0) { close(fd); continue; }
                close(c[old].fd);
                c[old] = c[--nc_used];
            }

            if ((lkind[i] == 1 && nbd_open >= NBD_CONNS) ||
                (lkind[i] == 1 && nbd_greet(fd, nc) < 0)) {
                close(fd);
                continue;
            }
            c[nc_used].fd     = fd;
            c[nc_used].is_nbd = (lkind[i] == 1);
            c[nc_used].last   = t;
            nc_used++;
        }

        if (once && done) break;
    }

    for (i = 0; i < nc_used; i++) close(c[i].fd);
    for (i = 0; i < nl; i++) close(lfd[i]);
    return 0;
}
