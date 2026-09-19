/*
 * ipc.h — AF_UNIX SOCK_SEQPACKET transport between the flight processes.
 *
 * ttcd listens; adsbd, the OBC and diagnostic tools connect (ADR-0005,
 * ADR-0006). SEQPACKET was chosen because it preserves message boundaries:
 * one send is one receive, so a GAMA frame needs no stream reassembly. That
 * reassembly is exactly what broke in the previous mission, where one lost
 * byte desynchronised the byte-by-byte parser for good
 * (ultima_missao/satellite/Module.cpp:65-79).
 *
 * Every call is non-blocking. The OBC runs a single-threaded 1 Hz loop, and
 * anything that can block in it stalls the whole on-board computer; ttcd runs
 * an epoll loop with the same constraint (AGENTS.md, hard rules 2 and 3).
 *
 * Linux only. Not part of common/: the ESP32 has no AF_UNIX.
 */

#ifndef TTEC_IPC_H
#define TTEC_IPC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IPC_OK        =  0,
    IPC_EMPTY     =  1,  /* recv: nothing queued. send: queue full, not sent */
    IPC_CLOSED    = -1,  /* the peer closed the connection                   */
    IPC_ERROR     = -2,  /* any other failure; errno is preserved            */
    IPC_TRUNCATED = -3   /* datagram larger than the buffer; discarded       */
} ipc_result_t;

/*
 * Creates a listening socket at path.
 *
 * A stale socket left at path by a previous run is removed first. Anything
 * else at that path — a regular file, a directory — is left alone and the
 * call fails with EEXIST, so a configuration mistake cannot delete a file.
 * Returns the listening fd (non-blocking, close-on-exec) or -1.
 */
int ipc_server_open(const char *path, int backlog);

/* Accepts one pending connection. Returns a non-blocking fd, or -1 with
 * errno EAGAIN when nothing is pending. */
int ipc_server_accept(int listen_fd);

/* Connects to a listening server. Returns a non-blocking fd or -1. Fails
 * immediately, never waits, if no server is listening. */
int ipc_client_connect(const char *path);

/*
 * Receives one datagram into buf.
 *
 * IPC_OK with *len set; IPC_EMPTY if nothing is queued; IPC_CLOSED when the
 * peer has gone. A datagram longer than cap is discarded and reported as
 * IPC_TRUNCATED rather than delivered cut short. Zero-length datagrams are
 * never sent by this protocol, so a zero-length read means the peer closed.
 */
ipc_result_t ipc_recv(int fd, uint8_t *buf, size_t cap, size_t *len);

/*
 * Sends one datagram without blocking.
 *
 * IPC_EMPTY means the peer's queue is full and the datagram was NOT sent: the
 * caller decides whether to drop it (and count the drop) or wait for POLLOUT.
 * Never raises SIGPIPE.
 */
ipc_result_t ipc_send(int fd, const uint8_t *buf, size_t len);

const char *ipc_result_name(ipc_result_t r);

#ifdef __cplusplus
}
#endif

#endif /* TTEC_IPC_H */
