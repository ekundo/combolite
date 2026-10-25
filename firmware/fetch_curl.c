/* Скачивание для моста Gopher на маке, в Linux и Windows: curl в трубе (fetch.h).
 *
 * Без новых библиотек: curl есть везде, где живут стенды и эмуляторы (в
 * Windows -- свой, с Windows 10). Труба не блокирует модем: в POSIX она
 * неблокирующая, в Windows прежде чтения спрашиваем, сколько в ней есть
 * (PeekNamedPipe). Ошибка HTTP -- curl с -f молчит и выходит с кодом 22;
 * тогда считаем, что файла нет (404), а прочие коды выхода -- что не дошли (0).
 *
 * В Windows curl запускается сам, без cmd.exe и _popen: у оконной программы
 * (Emu80) нет консоли, и _popen открывал бы на каждую загрузку окно cmd.exe.
 * CreateProcess с CREATE_NO_WINDOW окна не открывает. */

#include "fetch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define QUOTE  "\""                     /* разбор командной строки Windows: адрес в двойных кавычках */
#define CURL   "curl.exe"
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#define QUOTE  "'"
#define CURL   "curl"
#endif

#ifdef _WIN32
static HANDLE pipe_h;                   /* наш конец трубы, из него читаем */
static HANDLE proc_h;                   /* процесс curl */
#define RUNNING (pipe_h != 0)
#else
static FILE *pipe_f;
#define RUNNING (pipe_f != 0)
#endif
static int   status;
static int   state;                     /* 0 -- нет, 1 -- идёт, 2 -- кончилось, 3 -- не вышло */

#ifdef _WIN32
static int spawn(char *cmd)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd, wr, nul;
    BOOL ok;

    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   /* curl наследует только пишущий конец */
    nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    ok = CreateProcessA(0, cmd, 0, 0, TRUE, CREATE_NO_WINDOW, 0, 0, &si, &pi);
    CloseHandle(wr);                    /* иначе труба не кончится, когда curl выйдет */
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }
    CloseHandle(pi.hThread);
    pipe_h = rd;
    proc_h = pi.hProcess;
    return 0;
}

/* Код выхода curl; -1 -- неизвестен. */
static int finish(void)
{
    DWORD code = (DWORD)-1;
    WaitForSingleObject(proc_h, 5000);
    if (!GetExitCodeProcess(proc_h, &code) || code == STILL_ACTIVE) code = (DWORD)-1;
    CloseHandle(proc_h);
    CloseHandle(pipe_h);
    proc_h = pipe_h = 0;
    return (int)code;
}
#endif

int fetch_open(const char *url)
{
    char cmd[1024], full[768];
    const char *base = getenv("COMBO_FETCH_BASE");

    if (RUNNING) return -1;
    if (strchr(url, '\'') || strchr(url, '"')) return -1;   /* адрес идёт в командную строку в кавычках */
    full[0] = 0;
    if (base && *base) {                /* стенд: https://хост/путь -> base/путь */
        const char *p = strstr(url, "://");
        p = p ? strchr(p + 3, '/') : 0;
        snprintf(full, sizeof full, "%s%s", base, p ? p : "/");
        url = full;
    }
#ifdef _WIN32
    snprintf(cmd, sizeof cmd,
             CURL " -s -f -L --max-redirs 5 --max-time 60 --max-filesize %d -A ComboLite "
             QUOTE "%s" QUOTE, FETCH_MAX, url);
    if (spawn(cmd)) return -1;
#else
    snprintf(cmd, sizeof cmd,
             CURL " -s -f -L --max-redirs 5 --max-time 60 --max-filesize %d -A ComboLite "
             QUOTE "%s" QUOTE " 2>/dev/null", FETCH_MAX, url);
    pipe_f = popen(cmd, "r");
    if (!pipe_f) return -1;
    fcntl(fileno(pipe_f), F_SETFL, fcntl(fileno(pipe_f), F_GETFL, 0) | O_NONBLOCK);
#endif
    status = 0;
    state = 1;
    return 0;
}

/* Сколько байт трубы можно прочесть, не блокируясь: >0, 0 -- пока нечего,
 * -1 -- труба кончилась (curl вышел). */
static int pipe_read(uint8_t *buf, int max)
{
#ifdef _WIN32
    DWORD avail = 0, got = 0;
    if (!PeekNamedPipe(pipe_h, 0, 0, 0, &avail, 0)) return -1;  /* писатель закрыл, всё прочитано */
    if (!avail) return 0;
    if ((DWORD)max > avail) max = (int)avail;
    if (!ReadFile(pipe_h, buf, (DWORD)max, &got, 0)) return -1;
    return (int)got;
#else
    ssize_t n = read(fileno(pipe_f), buf, (size_t)max);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
#endif
}

int fetch_read(uint8_t *buf, int max)
{
    int n, code;

    if (state == 2) return FETCH_DONE;
    if (state != 1 || !RUNNING) return FETCH_FAIL;
    n = pipe_read(buf, max);
    if (n > 0) return n;
    if (n == 0) return 0;
#ifdef _WIN32
    code = finish();                    /* конец трубы: как вышел curl */
#else
    {
        int rc = pclose(pipe_f);
        code = (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
        pipe_f = 0;
    }
#endif
    if (code == 0) { state = 2; return FETCH_DONE; }
    status = code == 22 ? 404 : 0;
    state = 3;
    return FETCH_FAIL;
}

int fetch_status(void) { return status; }

void fetch_close(void)
{
#ifdef _WIN32
    if (RUNNING) {                      /* прерванный: ждать докачки незачем */
        TerminateProcess(proc_h, 1);
        finish();
    }
#else
    if (pipe_f) pclose(pipe_f);         /* ждёт curl: на прерванном это доли секунды */
    pipe_f = 0;
#endif
    state = 0;
}
