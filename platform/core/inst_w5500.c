/* inst_w5500.c - see inst_w5500.h */
#include "inst_w5500.h"

#define LPORT_FIRST 40000u          /* below the I/O boards' 49152.. range */
#define LPORT_LAST  48999u

static uint8_t sock(unsigned c) { return (uint8_t)(INST_W5500_SOCK0 + c); }
static inst_w5500_t *me(inst_net_t *n) { return (inst_w5500_t *)n->ctx; }

static bool ready(inst_net_t *n) { return iob_w5500_ready(me(n)->io); }

static bool connect(inst_net_t *n, unsigned c, uint32_t ip, uint16_t port) {
    inst_w5500_t *t = me(n);
    if (c > INST_CONN_MAX) return false;
    t->lport = (uint16_t)(t->lport < LPORT_FIRST || t->lport >= LPORT_LAST ? LPORT_FIRST : t->lport + 1u);
    return w5500_tcp_connect_start(&t->io->chip, sock(c), ip, port, t->lport);
}

static uint8_t state(inst_net_t *n, unsigned c) {
    switch (w5500_sock_state(&me(n)->io->chip, sock(c))) {
    case W5500_SOCK_ESTABLISHED: return INST_L_UP;
    case W5500_SOCK_INIT: case W5500_SOCK_SYNSENT: return INST_L_CONNECTING;
    default: return INST_L_CLOSED;     /* CLOSED, CLOSE_WAIT (the instrument closed it) */
    }
}

static bool send(inst_net_t *n, unsigned c, const uint8_t *buf, uint16_t len) {
    return w5500_send(&me(n)->io->chip, sock(c), buf, len, 20u);   /* a request fits the free buffer at once */
}

static uint16_t recv(inst_net_t *n, unsigned c, uint8_t *buf, uint16_t cap) {
    return w5500_recv(&me(n)->io->chip, sock(c), buf, cap);
}

static void close_(inst_net_t *n, unsigned c) { w5500_close(&me(n)->io->chip, sock(c)); }

void inst_w5500_init(inst_w5500_t *t, iob_w5500_t *io) {
    t->io = io;
    t->lport = 0;
    t->net = (inst_net_t){ .ready = ready, .connect = connect, .state = state, .send = send,
                           .recv = recv, .close = close_, .ctx = t };
}
