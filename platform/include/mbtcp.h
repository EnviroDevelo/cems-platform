/* mbtcp.h - Modbus TCP: MBAP framing and the server loop (design spec: port 502,
 * PLC and browsers on the same Ethernet port; 2-4 Modbus clients).
 *
 * The server runs in the Modbus task: mbtcp_poll() waits up to timeout_ms for
 * activity, serves every complete request, and returns so the task can check
 * in with the watchdog supervisor.
 *
 *  - Up to MBTCP_MAX_CLIENTS connections. When full, a new connection replaces
 *    the client that has been silent longest (a PLC that rebooted without
 *    closing its socket would otherwise hold a slot for ever).
 *  - Optional client filter: parameter modbus_allow_ip (0 = any).
 *  - Each valid request counts as remote activity for the remote-link watchdog.
 *  - A frame with a protocol ID other than 0 or a bad length closes the connection. */
#ifndef MBTCP_H
#define MBTCP_H
#include <stdint.h>
#include <stdbool.h>

#ifndef MBTCP_MAX_CLIENTS
#define MBTCP_MAX_CLIENTS 4
#endif
#define MBTCP_FRAME_MAX   260u        /* MBAP 7 + PDU 253 */

typedef struct {
    uint8_t  buf[MBTCP_FRAME_MAX];
    uint16_t n;
} mbtcp_rx_t;

/* Feed received bytes. Complete requests are answered into out (several may be
 * appended). *used says how much of in was consumed: call again with the rest
 * after sending, until all is used. Returns the bytes to send, or -1 if the
 * stream is not Modbus TCP (close the connection). out must hold at least one
 * frame (MBTCP_FRAME_MAX). */
int mbtcp_feed(mbtcp_rx_t *rx, const uint8_t *in, uint16_t len, uint16_t *used, uint8_t *out, uint16_t cap);

typedef struct {
    int        fd;
    uint32_t   ip;              /* host byte order */
    uint32_t   last_ms;
    mbtcp_rx_t rx;
} mbtcp_client_t;

typedef struct {
    int            lfd;
    mbtcp_client_t c[MBTCP_MAX_CLIENTS];
    uint32_t       accepted, refused_ip, evicted, closed, frames, protocol_errors;
    uint32_t       last_frame_ms;   /* bsp_ms() of the last request answered (frames > 0) */
    uint16_t       port;
} mbtcp_server_t;

int  mbtcp_open(mbtcp_server_t *s, uint16_t port);   /* 0 = listening */
void mbtcp_poll(mbtcp_server_t *s, uint32_t timeout_ms);
void mbtcp_close_all(mbtcp_server_t *s);
int  mbtcp_clients(const mbtcp_server_t *s);
/* The server opened last (the Modbus task's), for /api/info; NULL before one is listening. */
const mbtcp_server_t *mbtcp_main(void);

#endif
