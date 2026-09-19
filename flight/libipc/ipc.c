#include "ipc.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int fill_address(struct sockaddr_un *addr, const char *path)
{
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(addr->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, n + 1);
    return 0;
}

int ipc_server_open(const char *path, int backlog)
{
    struct sockaddr_un addr;
    if (fill_address(&addr, path) != 0) {
        return -1;
    }

    /* Remove a stale socket from a previous run, but only a socket. */
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            errno = EEXIST;
            return -1;
        }
        if (unlink(path) != 0) {
            return -1;
        }
    } else if (errno != ENOENT) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, backlog) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int ipc_server_accept(int listen_fd)
{
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0 || errno != EINTR) {
            return fd;
        }
    }
}

int ipc_client_connect(const char *path)
{
    struct sockaddr_un addr;
    if (fill_address(&addr, path) != 0) {
        return -1;
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    /* On AF_UNIX a connect either completes at once or fails; EAGAIN here
     * means the server's backlog is full, which the caller retries later
     * like any other failure. */
    for (;;) {
        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        if (errno != EINTR) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }
}

ipc_result_t ipc_recv(int fd, uint8_t *buf, size_t cap, size_t *len)
{
    struct iovec iov = { .iov_base = buf, .iov_len = cap };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    for (;;) {
        ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
        if (n > 0) {
            if (msg.msg_flags & MSG_TRUNC) {
                return IPC_TRUNCATED;
            }
            *len = (size_t)n;
            return IPC_OK;
        }
        if (n == 0) {
            return IPC_CLOSED;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return IPC_EMPTY;
        }
        if (errno == ECONNRESET) {
            return IPC_CLOSED;
        }
        return IPC_ERROR;
    }
}

ipc_result_t ipc_send(int fd, const uint8_t *buf, size_t len)
{
    for (;;) {
        ssize_t n = send(fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n >= 0) {
            /* SEQPACKET sends are atomic: all of the datagram or none. */
            return IPC_OK;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return IPC_EMPTY;
        }
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
            return IPC_CLOSED;
        }
        return IPC_ERROR;
    }
}

const char *ipc_result_name(ipc_result_t r)
{
    switch (r) {
    case IPC_OK:        return "ok";
    case IPC_EMPTY:     return "empty";
    case IPC_CLOSED:    return "closed";
    case IPC_ERROR:     return "error";
    case IPC_TRUNCATED: return "truncated";
    default:            return "unknown";
    }
}
