/* nodesim_main.c - the NODE role (plat_node.h) on a PC: the platform's own node runtime
 * (platform/net/plat_node.c over POSIX sockets) and node core (node/), with the pins on a console.
 * The partner of build/netsim (the CORE) in tests/integration/node_role_test.py, and a board for a
 * bench DK when no Nucleo is at hand.
 *
 *   ./build/nodesim [tcp_port] [disc_port]       (defaults 502 and 50223)
 *   CEMS_NODE_DISC_TO=a.b.c.d  where discovery answers go (default the limited broadcast)
 *   CEMS_UID=24 hex digits     the board's unique ID (as netsim)
 * It starts as board 1 at 192.168.1.61/24, any supervisor (PLAT_NODE_DEFAULT_IP), like the Nucleo.
 * Console: in <mask> (inputs 1-8), show, quit. Output changes are printed as they happen. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "plat_node.h"
#include "plat_io.h"
#include "bsp.h"
#include "plat_diag.h"

plat_diag_t plat_diag;                   /* only last_reset_cause is read: 0, unknown */

static pthread_mutex_t crit;
void plat_host_crit_enter(void) { pthread_mutex_lock(&crit); }
void plat_host_crit_exit(void)  { pthread_mutex_unlock(&crit); }

static struct timespec t0;
static volatile uint32_t sim_di;
static uint64_t shown_do = ~0ull;
static plat_io_out_t sim_out;
static uint32_t backup[16];

uint32_t bsp_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}
void bsp_io_read(plat_io_in_t *in) { in->di = sim_di; }
void bsp_io_write(const plat_io_out_t *out) { sim_out = *out; }
bool bsp_hw_straps(uint8_t *rev) { (void)rev; return false; }
uint32_t bsp_backup_read(unsigned idx) { return idx < 16u ? backup[idx] : 0u; }
bool bsp_backup_write(unsigned idx, uint32_t v) { if (idx >= 16u) return false; backup[idx] = v; return true; }
void bsp_chip_uid(uint8_t uid[12]) {
    static const uint8_t dflt[12] = { 0x4e, 0x4f, 0x44, 0x45, 0x53, 0x49, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x01 };
    const char *e = getenv("CEMS_UID");
    for (int k = 0; k < 12; ++k) {
        unsigned b;
        uid[k] = e && strlen(e) == 24 && sscanf(e + 2 * k, "%2x", &b) == 1 ? (uint8_t)b : dflt[k];
    }
}

static plat_node_t nd;
static volatile int running = 1;

static void show(void) {
    const node_t *n = &nd.node;
    printf("node: address %u, ip %u.%u.%u.%u, master %u.%u.%u.%u, gate %s, outputs 0x%04x, relays %u, inputs 0x%02x, "
           "frames %u, connections %u\n", n->address,
           nd.net.cfg.ip >> 24, (nd.net.cfg.ip >> 16) & 255u, (nd.net.cfg.ip >> 8) & 255u, nd.net.cfg.ip & 255u,
           nd.net.cfg.master_ip >> 24, (nd.net.cfg.master_ip >> 16) & 255u, (nd.net.cfg.master_ip >> 8) & 255u,
           nd.net.cfg.master_ip & 255u, n->gate ? "open" : "closed", n->out_applied, n->relay_applied,
           (unsigned)sim_di & 255u, (unsigned)n->frames, (unsigned)nd.accepted);
    fflush(stdout);
}

static void *console(void *a) {
    (void)a;
    char line[128];
    size_t have = 0;
    for (;;) {
        ssize_t r = read(0, line + have, sizeof line - 1 - have);
        if (r <= 0) { running = 0; return NULL; }
        have += (size_t)r;
        line[have] = 0;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = 0;
            plat_host_crit_enter();
            if (!strncmp(line, "in ", 3)) sim_di = (uint32_t)strtoul(line + 3, NULL, 0) & 255u;
            else if (!strcmp(line, "show")) show();
            else if (!strcmp(line, "quit")) running = 0;
            plat_host_crit_exit();
            memmove(line, nl + 1, strlen(nl + 1) + 1);
            have = strlen(line);
            if (!running) return NULL;
        }
    }
}

int main(int argc, char **argv) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_mutexattr_t ma;               /* plat_node_poll takes it again inside */
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&crit, &ma);
    plat_node_init(&nd, bsp_ms());
    if (argc > 1) nd.tcp_port = (uint16_t)atoi(argv[1]);
    if (argc > 2) nd.disc_port = (uint16_t)atoi(argv[2]);
    unsigned a, b, c, d;
    const char *e = getenv("CEMS_NODE_DISC_TO");
    if (e && sscanf(e, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) nd.disc_to = a << 24 | b << 16 | c << 8 | d;
    printf("nodesim: the node role, Modbus TCP on port %u, discovery on port %u\n", nd.tcp_port, nd.disc_port);
    show();
    pthread_t th;
    pthread_create(&th, NULL, console, NULL);
    while (running) {
        plat_host_crit_enter();
        plat_node_poll(&nd, bsp_ms(), 0);
        if (sim_out.do_bits != shown_do) {
            shown_do = sim_out.do_bits;
            printf("node: outputs %s0x%04x, relays %u\n", shown_do ? "on " : "all off ", (unsigned)(shown_do & 0xFFFFu),
                   (unsigned)(shown_do >> 16) & 3u);
            fflush(stdout);
        }
        plat_host_crit_exit();
        struct timespec ts = { 0, 1000000L };
        nanosleep(&ts, NULL);
    }
    plat_node_close(&nd);
    return 0;
}
