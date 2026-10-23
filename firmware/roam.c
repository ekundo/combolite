#include "roam.h"

int roam_decide(int cur, int best, int same, int64_t since_s, int soft)
{
    const int poor  = soft ? ROAM_POOR_SOFT  : ROAM_POOR;
    const int gain  = soft ? ROAM_GAIN_SOFT  : ROAM_GAIN;
    const int pause = soft ? ROAM_PAUSE_SOFT : ROAM_PAUSE_S;

    /* Порядок проверок — от дешёвых к дорогим по последствиям, и каждая
       отвечает на свой вопрос:

       связь ещё годная?      — тогда не трогаем ничего;
       давно ли переходили?   — иначе будем метаться;
       это не та же точка?    — переходить на себя бессмысленно;
       выигрыш настоящий?     — иначе меняем шило на мыло. */
    /* Само пороговое значение — ещё терпимо: ROAM_POOR описан как «ХУЖЕ этого
       мешает», и на самой границе связь пока годная. Граница на единицу, и
       ошибиться в ней проще всего. */
    if (cur >= poor)                  return 0;
    if (since_s < pause)              return 0;
    if (same)                         return 0;
    if (best < cur + gain)            return 0;
    return 1;
}
