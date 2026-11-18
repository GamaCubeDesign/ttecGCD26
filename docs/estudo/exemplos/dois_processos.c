/*
 * dois_processos.c — the smallest version of what ttcd and adsbd do:
 * two processes, one AF_UNIX SOCK_SEQPACKET socket between them, and a
 * loop that waits on several file descriptors at once without blocking
 * on any one of them.
 *
 *   cc -std=c11 -Wall -Wextra -o dois_processos dois_processos.c
 *   ./dois_processos
 *
 * The parent plays ttcd (the hub): it waits on the socket and on a 1 s
 * timer with epoll. The child plays adsbd: it sends three "snapshots" and
 * exits. When the child exits, the kernel closes its end, and the parent
 * reads end-of-file at once: that is how ttcd learns that a peer died.
 *
 * Study notes, not flight code (docs/estudo/03-decisoes-de-arquitetura.md, question 27).
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

static void child(int fd)
{
    for (int i = 1; i <= 3; i++) {
        char msg[32];
        int n = snprintf(msg, sizeof msg, "retrato %d", i);
        send(fd, msg, (size_t)n, 0);        /* one send = one message */
        sleep(1);                          /* fine here: this child has no loop to stall */
    }
    close(fd);                             /* or just exit: the kernel closes it */
    _exit(0);
}

int main(void)
{
    /* 1. Two connected sockets. ttcd uses a named one instead
     *    (socket + bind + listen at /run/gama/ttec.sock), so that processes
     *    started separately by systemd can find it. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) { perror("socketpair"); return 1; }

    /* 2. A second process: fork() copies this one; both continue from here. */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) { close(sv[0]); child(sv[1]); }
    close(sv[1]);

    /* 3. The parent's loop: a timer and the socket, both just fds. */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec every_1s = { .it_interval = {1, 0}, .it_value = {1, 0} };
    timerfd_settime(tfd, 0, &every_1s, NULL);

    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sv[0] };
    epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev);
    ev.data.fd = tfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);

    for (;;) {
        struct epoll_event got[4];
        int n = epoll_wait(ep, got, 4, -1);      /* the only place that waits */
        for (int i = 0; i < n; i++) {
            if (got[i].data.fd == tfd) {
                uint64_t ticks;
                if (read(tfd, &ticks, sizeof ticks) == (ssize_t)sizeof ticks) {
                    printf("[pai]   tic: hora de mandar HK\n");
                }
            } else {
                char buf[64];
                ssize_t r = recv(sv[0], buf, sizeof buf - 1, MSG_DONTWAIT);
                if (r > 0) {
                    buf[r] = '\0';
                    printf("[pai]   recebi do filho: \"%s\"\n", buf);
                } else if (r == 0) {
                    printf("[pai]   o filho fechou o socket: morreu ou terminou\n");
                    waitpid(pid, NULL, 0);           /* collect it, or it stays a zombie */
                    return 0;
                }
            }
        }
    }
}
