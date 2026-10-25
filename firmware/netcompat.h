/* Сокеты модема (modem.c) и списка BBS (bbslist.c) там, где они бывают:
 * на плате (lwip), на маке и в Linux (стенды, v06x, Emu80) -- POSIX как есть,
 * в Windows (Emu80 под MinGW) -- Winsock.
 *
 * Отличия Winsock, ради которых всё это: сокет -- не файл (closesocket, а не
 * close), неблокирующий режим -- ioctlsocket, poll -- WSAPoll, ошибки -- не в
 * errno, а в WSAGetLastError, таймаут чтения -- миллисекунды числом, а не
 * timeval, и до первого сокета -- WSAStartup. Сокет здесь -- int, как в
 * POSIX: в Windows SOCKET -- число побольше, но на деле маленькое, а
 * INVALID_SOCKET в int -- те же -1. */
#ifndef COMBO_NETCOMPAT_H
#define COMBO_NETCOMPAT_H

#ifdef _WIN32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600             /* WSAPoll -- с Vista */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

static inline int net_start(void)
{
    static int done;
    WSADATA w;
    if (!done) done = WSAStartup(MAKEWORD(2, 2), &w) == 0;
    return done;
}
static inline int  net_socket(int fam, int type, int proto)
{
    SOCKET s;
    net_start();
    s = socket(fam, type, proto);
    return s == INVALID_SOCKET ? -1 : (int)s;
}
static inline void net_close(int s) { closesocket((SOCKET)s); }
static inline void net_nonblock(int s) { u_long on = 1; ioctlsocket((SOCKET)s, FIONBIO, &on); }
/* Неблокирующий вызов «пока нельзя»: connect ещё идёт, send или recv ждут. */
static inline int  net_again(void)
{
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
static inline int  net_writable(int s)          /* connect закончился (удачно или нет) */
{
    WSAPOLLFD pf;
    pf.fd = (SOCKET)s; pf.events = POLLOUT; pf.revents = 0;
    return WSAPoll(&pf, 1, 0) > 0;
}
static inline int  net_error(int s)
{
    int err = 0, el = sizeof err;
    getsockopt((SOCKET)s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
    return err;
}
static inline void net_timeouts(int s, int sec)
{
    DWORD ms = (DWORD)sec * 1000;
    setsockopt((SOCKET)s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
    setsockopt((SOCKET)s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof ms);
}
static inline int  net_send(int s, const void *p, int n) { return send((SOCKET)s, (const char *)p, n, 0); }
static inline int  net_recv(int s, void *p, int n) { return recv((SOCKET)s, (char *)p, n, 0); }

#else

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static inline int  net_start(void) { return 1; }
static inline int  net_socket(int fam, int type, int proto) { return socket(fam, type, proto); }
static inline void net_close(int s) { close(s); }
static inline void net_nonblock(int s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
static inline int  net_again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }
static inline int  net_writable(int s)
{
    struct pollfd pf = { s, POLLOUT, 0 };
    return poll(&pf, 1, 0) > 0;
}
static inline int  net_error(int s)
{
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
    return err;
}
static inline void net_timeouts(int s, int sec)
{
    struct timeval tv;
    tv.tv_sec = sec; tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}
static inline int  net_send(int s, const void *p, int n) { return (int)send(s, p, (size_t)n, 0); }
static inline int  net_recv(int s, void *p, int n) { return (int)recv(s, p, (size_t)n, 0); }

#endif

#endif
