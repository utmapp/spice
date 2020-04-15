/*
   Copyright (C) 2018 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

/* Small compatibility layer for sockets, mostly to make easier portability
 * for Windows but without loosing performances under Unix, the most supported
 * system */
#ifndef RED_SYS_SOCKET_H_
#define RED_SYS_SOCKET_H_

#ifndef _WIN32
#  include <sys/socket.h>

#define socket_read(sock, buf, len) read(sock, buf, len)
#define socket_write(sock, buf, len) write(sock, buf, len)
#define socket_writev(sock, iov, n) writev(sock, iov, n)
#define socket_close(sock) close(sock)

#else
#  include <winsock2.h>
#  include <windows.h>
typedef int socklen_t;

// this definition is ABI compatible with WSABUF
struct iovec {
    u_long iov_len;
    void FAR *iov_base;
};

void socket_win32_set_errno(void);

static inline ssize_t socket_read(int sock, void *buf, size_t count)
{
    ssize_t res = recv(sock, buf, count, 0);
    if (res < 0) {
        socket_win32_set_errno();
    }
    return res;
}

static inline ssize_t socket_write(int sock, const void *buf, size_t count)
{
    ssize_t res = send(sock, buf, count, 0);
    if (res < 0) {
        socket_win32_set_errno();
    }
    return res;
}

static inline ssize_t socket_writev(int sock, const struct iovec *iov, int n_iov)
{
    DWORD sent;
    int res = WSASend(sock, (LPWSABUF) iov, n_iov, &sent, 0, NULL, NULL);
    if (res) {
        socket_win32_set_errno();
        return -1;
    }
    return sent;
}

#define socket_close(sock) closesocket(sock)

#define SHUT_RDWR SD_BOTH

static inline int
socket_getsockopt(int sock, int lvl, int type, void *value, socklen_t *len)
{
    int res = getsockopt(sock, lvl, type, value, len);
    if (res < 0) {
        socket_win32_set_errno();
    }
    return res;
}
#undef getsockopt
#define getsockopt socket_getsockopt

static inline int
socket_setsockopt(int sock, int lvl, int type, const void *value, socklen_t len)
{
    int res = setsockopt(sock, lvl, type, value, len);
    if (res < 0) {
        socket_win32_set_errno();
    }
    return res;
}
#undef setsockopt
#define setsockopt socket_setsockopt

static inline int
socket_listen(int sock, int backlog)
{
    int res = listen(sock, backlog);
    if (res < 0) {
        socket_win32_set_errno();
    }
    return res;
}
#undef listen
#define listen socket_listen

static inline int
socket_bind(int sock, const struct sockaddr *addr, int addrlen)
{
    int res = bind(sock, addr, addrlen);
    if (res < 0) {
        socket_win32_set_errno();
    }
    return res;
}
#undef bind
#define bind socket_bind

static inline int
socket_accept(int sock, struct sockaddr *addr, int *addrlen)
{
    int res = accept(sock, addr, addrlen);
    if (res < 0) {
        socket_win32_set_errno();
    }
    return res;
}
#undef accept
#define accept socket_accept

int socket_newpair(int type, int protocol, int sv[2]);
#define socketpair(family, type, protocol, sv) socket_newpair(type, protocol, sv)
#endif

#if defined(SO_NOSIGPIPE) && defined(__APPLE__)
#define MSG_NOSIGNAL 0
#endif

#endif // RED_SYS_SOCKET_H_
