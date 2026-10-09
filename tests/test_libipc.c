/*
 * SEQPACKET transport. The acceptance criterion from PLANO phase 2.1: two
 * processes exchange 10 000 frames with no loss and no blocking call, and a
 * receive on an empty socket returns immediately.
 */

#define _GNU_SOURCE
#include "test_util.h"
#include "ipc.h"
#include "gama_frame.h"
#include "gama_bytes.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define N_FRAMES 10000

static char dir[64];
static char sock_path[128];

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Sends one frame, waiting for POLLOUT if the queue is full. The test's
 * sender is patient on purpose: the point is to show the transport loses
 * nothing when the sender honours back-pressure. */
static int send_frame(int fd, uint16_t seq)
{
    uint8_t payload[4], frame[GAMA_FRAME_MAX_TOTAL];
    gama_put_u32(payload, (uint32_t)seq * 7u + 1u);
    int n = gama_frame_encode(frame, sizeof(frame), GAMA_FRAME_IPC_STAT, seq, payload, 4);
    for (;;) {
        ipc_result_t r = ipc_send(fd, frame, (size_t)n);
        if (r == IPC_OK) { return 0; }
        if (r != IPC_EMPTY) { return -1; }
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        poll(&p, 1, 1000);
    }
}

/* Receives one frame, waiting for POLLIN, and checks it is exactly the
 * expected one: right sequence, valid CRC, right contents. */
static int recv_frame(int fd, uint16_t want_seq)
{
    uint8_t buf[GAMA_FRAME_MAX_TOTAL + 1];
    size_t len;
    for (;;) {
        ipc_result_t r = ipc_recv(fd, buf, sizeof(buf), &len);
        if (r == IPC_OK) { break; }
        if (r != IPC_EMPTY) { return -1; }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 2000) == 0) { return -2; }   /* would be a loss */
    }
    gama_frame_t f;
    if (gama_frame_decode(buf, len, &f) <= 0) { return -3; }
    if (f.seq != want_seq) { return -4; }
    if (gama_get_u32(f.payload) != (uint32_t)want_seq * 7u + 1u) { return -5; }
    return 0;
}

static int child_client(void)
{
    int fd = -1;
    for (int i = 0; i < 200 && fd < 0; i++) {
        fd = ipc_client_connect(sock_path);
        if (fd < 0) { usleep(5000); }
    }
    if (fd < 0) { return 10; }
    for (uint32_t i = 0; i < N_FRAMES; i++) {
        if (send_frame(fd, (uint16_t)i) != 0) { return 11; }
    }
    for (uint32_t i = 0; i < N_FRAMES; i++) {
        if (recv_frame(fd, (uint16_t)i) != 0) { return 12; }
    }
    close(fd);
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    snprintf(dir, sizeof(dir), "/tmp/ttec-ipc-XXXXXX");
    if (mkdtemp(dir) == NULL) { perror("mkdtemp"); return 1; }
    snprintf(sock_path, sizeof(sock_path), "%s/ttec.sock", dir);

    TEST_GROUP("libipc: server opens and a client connects");
    int lfd = ipc_server_open(sock_path, 8);
    CHECK(lfd >= 0);

    TEST_GROUP("libipc: accept with nothing pending does not block");
    {
        double t0 = now_s();
        CHECK_EQ_INT(ipc_server_accept(lfd), -1);
        CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
        CHECK(now_s() - t0 < 0.01);
    }

    TEST_GROUP("libipc: 10 000 frames each way between two processes, no loss");
    {
        pid_t pid = fork();
        if (pid == 0) {
            _exit(child_client());
        }
        int cfd = -1;
        for (int i = 0; i < 400 && cfd < 0; i++) {
            cfd = ipc_server_accept(lfd);
            if (cfd < 0) { usleep(5000); }
        }
        CHECK(cfd >= 0);

        double t0 = now_s();
        int bad_rx = 0, bad_tx = 0;
        for (uint32_t i = 0; i < N_FRAMES; i++) {
            if (recv_frame(cfd, (uint16_t)i) != 0) { bad_rx++; break; }
        }
        for (uint32_t i = 0; i < N_FRAMES; i++) {
            if (send_frame(cfd, (uint16_t)i) != 0) { bad_tx++; break; }
        }
        int status = 0;
        waitpid(pid, &status, 0);
        double dt = now_s() - t0;

        CHECK_EQ_INT(bad_rx, 0);
        CHECK_EQ_INT(bad_tx, 0);
        CHECK(WIFEXITED(status));
        CHECK_EQ_INT(WEXITSTATUS(status), 0);
        printf("  %d frames each way in %.3f s\n", N_FRAMES, dt);

        TEST_GROUP("libipc: receive on an empty socket returns at once");
        {
            uint8_t buf[256]; size_t len;
            double t1 = now_s();
            /* The child has exited, so the socket reports closed, not empty;
             * reach an empty-but-open socket with a fresh pair instead. */
            int a = ipc_client_connect(sock_path);
            int b = -1;
            for (int i = 0; i < 100 && b < 0; i++) { b = ipc_server_accept(lfd); }
            CHECK(a >= 0 && b >= 0);
            t1 = now_s();
            CHECK_EQ_INT(ipc_recv(b, buf, sizeof(buf), &len), IPC_EMPTY);
            CHECK(now_s() - t1 < 0.001);

            TEST_GROUP("libipc: message boundaries survive");
            {
                const uint8_t m1[] = { 1, 2, 3 };
                const uint8_t m2[] = { 4, 5, 6, 7, 8 };
                CHECK_EQ_INT(ipc_send(a, m1, sizeof(m1)), IPC_OK);
                CHECK_EQ_INT(ipc_send(a, m2, sizeof(m2)), IPC_OK);
                CHECK_EQ_INT(ipc_recv(b, buf, sizeof(buf), &len), IPC_OK);
                CHECK_EQ_INT(len, 3);
                CHECK_EQ_INT(ipc_recv(b, buf, sizeof(buf), &len), IPC_OK);
                CHECK_EQ_INT(len, 5);
                CHECK_MEM_EQ(buf, m2, 5);
            }

            TEST_GROUP("libipc: an oversized datagram is reported, not cut short");
            {
                uint8_t big[300];
                memset(big, 0xAB, sizeof(big));
                CHECK_EQ_INT(ipc_send(a, big, sizeof(big)), IPC_OK);
                CHECK_EQ_INT(ipc_recv(b, buf, sizeof(buf), &len), IPC_TRUNCATED);
                /* And the next datagram is still delivered intact. */
                CHECK_EQ_INT(ipc_send(a, buf, 4), IPC_OK);
                CHECK_EQ_INT(ipc_recv(b, buf, sizeof(buf), &len), IPC_OK);
                CHECK_EQ_INT(len, 4);
            }

            TEST_GROUP("libipc: a closed peer is reported as closed");
            {
                close(a);
                CHECK_EQ_INT(ipc_recv(b, buf, sizeof(buf), &len), IPC_CLOSED);
                uint8_t one = 1;
                CHECK_EQ_INT(ipc_send(b, &one, 1), IPC_CLOSED);
                close(b);
            }
        }
        close(cfd);
    }

    TEST_GROUP("libipc: no server means an immediate failure, not a wait");
    {
        close(lfd);
        unlink(sock_path);
        double t0 = now_s();
        CHECK_EQ_INT(ipc_client_connect(sock_path), -1);
        CHECK(now_s() - t0 < 0.01);
    }

    TEST_GROUP("libipc: the server never deletes a file that is not a socket");
    {
        char victim[160];
        snprintf(victim, sizeof(victim), "%s/not-a-socket", dir);
        int f = open(victim, O_CREAT | O_WRONLY, 0600);
        CHECK(f >= 0);
        close(f);
        CHECK_EQ_INT(ipc_server_open(victim, 1), -1);
        CHECK_EQ_INT(errno, EEXIST);
        struct stat st;
        CHECK_EQ_INT(stat(victim, &st), 0);   /* still there */
        unlink(victim);
    }

    TEST_GROUP("libipc: a stale socket from a previous run is replaced");
    {
        int s1 = ipc_server_open(sock_path, 1);
        CHECK(s1 >= 0);
        close(s1);                 /* leaves the socket file behind */
        int s2 = ipc_server_open(sock_path, 1);
        CHECK(s2 >= 0);
        close(s2);
        unlink(sock_path);
    }

    TEST_GROUP("libipc: an over-long path is refused");
    {
        char longp[200];
        memset(longp, 'a', sizeof(longp) - 1);
        longp[sizeof(longp) - 1] = '\0';
        CHECK_EQ_INT(ipc_server_open(longp, 1), -1);
        CHECK_EQ_INT(errno, ENAMETOOLONG);
    }

    rmdir(dir);
    TEST_SUMMARY("test_libipc");
}
