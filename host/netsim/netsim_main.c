/* netsim_main.c - the platform on a PC with real sockets and real time:
 * scan (10 ms), store (20 ms), Modbus TCP and web server, each in its own thread
 * as on the RTOS. The parameter FRAM is a file, so settings survive restarts.
 *
 *   ./build/netsim [modbus_port] [http_port] [fram_file] [nor_file]
 *   (defaults 1502 8443 netsim_fram.bin netsim_nor.bin; the NOR file holds the logs)
 *   The web interface is HTTPS on the second port; plain HTTP on the port after it only redirects.
 * The two program-flash banks are netsim_fw.bin (next to the NOR file). A firmware
 * activation swaps them and exits with status 3 ("reset"); start it again to boot the
 * "new image". Environment: CEMS_FW_VERSION (the version this run reports),
 * CEMS_RESET_CAUSE (power, software, iwdg), CEMS_FW_TRIAL_MS (trial length),
 * CEMS_FW_PUBKEY (hex P-256 public key: acts as a signed production build).
 * Console (the touchscreen's stand-in): remote, local, set <id> <v>, fwyes / fwno (answer a held firmware restart).
 * Two simulated I/O boards sit on a simulated RS-485 bus at addresses 1 and 2 (enable
 * them with set 110 1 / set 120 1). Console: node <n> power|in|open|short|hot|wire|bfault|fielda|fieldb <value>
 * (n = 1 or 2; in/open/short/hot/wire take a bit mask, the rest 0 or 1), node <n> restart|wdg,
 * node <n> addr <a> (the board's own address; 0 = a new board, answering at 246).
 *
 * DI0 is driven by a pattern (8 s on, 4 s off) so values change on their own. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "scan_engine.h"
#include "plat_sys.h"
#include "param.h"
#include "param_store.h"
#include "modbus.h"
#include "mbtcp.h"
#include "http_api.h"
#include "http_server.h"
#include "mail.h"
#include "plat_clock.h"
#include "web_auth.h"
#include "plat_diag.h"
#include "plat_tasks.h"
#include "fw_update.h"
#include "plat_serial.h"
#include "tls.h"
#include "iobus.h"
#include "iob_node_sim.h"
#include "iob_tcp.h"
#include "iob_w5500.h"
#include "w5500_sim.h"
#include <math.h>

/* ---------------- critical sections ---------------- */
static pthread_mutex_t crit;
void plat_host_crit_enter(void) { pthread_mutex_lock(&crit); }
void plat_host_crit_exit(void)  { pthread_mutex_unlock(&crit); }

/* ---------------- BSP ---------------- */
static struct timespec t0;
static volatile uint32_t sim_di;
static plat_io_out_t sim_out;
static const char *fram_path = "netsim_fram.bin";
static uint8_t fram[PLAT_FRAM_SIZE];

void     bsp_init(void) { }
uint32_t bsp_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}
uint32_t bsp_cycles(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec); }
uint32_t bsp_cycles_per_us(void) { return 1000; }                      /* "cycles" are ns */
static long rtc_offset_s;                     /* set clock: an offset on the PC's local time */
bool bsp_rtc_read(bsp_rtc_t *r) {
    time_t now = time(NULL) + rtc_offset_s; struct tm tm; localtime_r(&now, &tm);
    *r = (bsp_rtc_t){ (uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday,
                      (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec, (uint8_t)(tm.tm_wday == 0 ? 7 : tm.tm_wday) };
    return true;
}
bool bsp_rtc_write(const bsp_rtc_t *t) {
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    uint32_t pc = plat_unix_from_civil((unsigned)tm.tm_year + 1900u, (unsigned)tm.tm_mon + 1u, (unsigned)tm.tm_mday,
                                       (unsigned)tm.tm_hour, (unsigned)tm.tm_min, (unsigned)tm.tm_sec);
    rtc_offset_s = (long)plat_unix_from_civil(t->year, t->month, t->day, t->hours, t->minutes, t->seconds) - (long)pc;
    return true;
}
void bsp_io_read(plat_io_in_t *in) { in->di = sim_di; }
void bsp_io_write(const plat_io_out_t *out) { sim_out = *out; }
void bsp_io_safe_state(void) { memset(&sim_out, 0, sizeof sim_out); }
void bsp_wdg_kick(void) { }
bsp_reset_cause_t bsp_reset_cause(void) {
    const char *c = getenv("CEMS_RESET_CAUSE");
    if (c && !strcmp(c, "software")) return BSP_RESET_SOFTWARE;
    if (c && !strcmp(c, "iwdg")) return BSP_RESET_IWDG;
    return BSP_RESET_POWER_ON;
}
void bsp_system_reset(void) { fprintf(stderr, "reset requested\n"); exit(3); }
void bsp_random(void *buf, uint32_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len) abort();
    fclose(f);
}

/* RS-485: two simulated I/O boards, guarded by the critical-section lock (the console changes them) */
static iob_simbus_t simbus;
static iob_node_t   simnode[2];
static uint16_t locked_xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t t) {
    (void)p;
    plat_host_crit_enter();
    uint16_t r = simbus.port.xfer(&simbus.port, tx, n, rx, cap, t);
    plat_host_crit_exit();
    return r;
}
static iob_port_t sim_rs485 = { .xfer = locked_xfer };
struct iob_port *bsp_rs485(void) { return &sim_rs485; }

/* Ethernet: the same two boards on a simulated network, or real boards over sockets when
 * CEMS_IOB_TCP is set (CEMS_IOB_TCP_PORT, CEMS_IOB_DISC_PORT and CEMS_IOB_DISC_TO=a.b.c.d change
 * the ports and where discovery goes; CEMS_IOB_TCP_HOST=a.b.c.d connects there whatever the
 * board's IP: the io-node virtual board, build/vnode_net, on 127.0.0.1) */
static iob_simnet_t simnet;
static iob_tcp_t    realnet;
/* the I/O port (iob_link 2, the default): a simulated W5500 in front of the same simulated network */
static w5500_sim_t  iochip;
static iob_w5500_t  ioport;
static void io_lock(bool on) { if (on) plat_host_crit_enter(); else plat_host_crit_exit(); }
static void io_wait(w5500_bus_t *b, uint32_t ms) { (void)b; struct timespec t = { 0, (long)ms * 1000000L }; nanosleep(&t, NULL); }
static uint16_t locked_net_xfer(iob_port_t *p, const uint8_t *tx, uint16_t n, uint8_t *rx, uint16_t cap, uint32_t t) {
    (void)p;
    plat_host_crit_enter();
    uint16_t r = simnet.port.xfer(&simnet.port, tx, n, rx, cap, t);
    plat_host_crit_exit();
    return r;
}
static void locked_net_target(iob_port_t *p, uint32_t ip) { (void)p; plat_host_crit_enter(); simnet.port.target(&simnet.port, ip); plat_host_crit_exit(); }
static bool locked_send(iob_dgram_t *d, const uint8_t *b, uint16_t n) {
    (void)d;
    plat_host_crit_enter();
    bool r = simnet.disc.send(&simnet.disc, b, n);
    plat_host_crit_exit();
    return r;
}
static uint16_t locked_recv(iob_dgram_t *d, uint8_t *b, uint16_t cap, uint32_t t) {
    (void)d;
    plat_host_crit_enter();
    uint16_t r = simnet.disc.recv(&simnet.disc, b, cap, t);
    plat_host_crit_exit();
    return r;
}
static iob_port_t  sim_tcp  = { .xfer = locked_net_xfer, .target = locked_net_target };
static iob_dgram_t sim_disc = { .send = locked_send, .recv = locked_recv };
static void eth_init(void) {
    if (getenv("CEMS_IOB_TCP")) {
        iob_tcp_init(&realnet);
        const char *e;
        if ((e = getenv("CEMS_IOB_TCP_PORT")) != NULL) realnet.tcp_port = (uint16_t)atoi(e);
        if ((e = getenv("CEMS_IOB_DISC_PORT")) != NULL) realnet.disc_port = (uint16_t)atoi(e);
        unsigned a, b, c, d;
        if ((e = getenv("CEMS_IOB_DISC_TO")) != NULL && sscanf(e, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
            realnet.disc_to = a << 24 | b << 16 | c << 8 | d;
        if ((e = getenv("CEMS_IOB_TCP_HOST")) != NULL && sscanf(e, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
            realnet.host = a << 24 | b << 16 | c << 8 | d;
        iob_init_eth(&realnet.port, &realnet.disc);
        fprintf(stderr, "netsim: I/O boards on Ethernet over sockets, Modbus TCP port %u, discovery port %u "
                "(the main-port link: \"set 102 1\")\n", realnet.tcp_port, realnet.disc_port);
        return;
    }
    iob_simnet_init(&simnet, param_tag_get(param_find(PARAM_ID_NET_IP)).u);
    simnet.now = bsp_ms;
    for (unsigned k = 0; k < 2; ++k) iob_simnet_add(&simnet, &simnode[k]);
    iob_init_eth(&sim_tcp, &sim_disc);
    w5500_sim_init(&iochip);
    w5500_sim_simnet(&iochip, &simnet);
    iochip.lock = io_lock;
    iochip.bus.ms = bsp_ms;
    iochip.bus.wait = io_wait;
    uint8_t uid[12], mac[6];
    bsp_chip_uid(uid);
    iob_w5500_mac(uid, mac);
    iob_w5500_init(&ioport, &iochip.bus, mac);
    iob_init_ioport(&ioport.port, &ioport.disc);
}

/* FRAM: RAM image written through to a file */
static int f_read(nvm_dev_t *d, uint32_t a, void *b, uint32_t n) { (void)d; if (a + n > sizeof fram) return -1; memcpy(b, fram + a, n); return 0; }
static int f_write(nvm_dev_t *d, uint32_t a, const void *b, uint32_t n) {
    (void)d; if (a + n > sizeof fram) return -1;
    memcpy(fram + a, b, n);
    FILE *f = fopen(fram_path, "r+b"); if (!f) f = fopen(fram_path, "w+b");
    if (!f) return -1;
    fseek(f, (long)a, SEEK_SET); size_t w = fwrite(b, 1, n, f); fclose(f);
    return w == n ? 0 : -1;
}
static nvm_dev_t fram_dev = { f_read, f_write, sizeof fram, true, NULL };

/* NOR flash for the logs: 1 MB, RAM image written through to a file */
#define NOR_SIZE (1024u * 1024u)
static const char *nor_path = "netsim_nor.bin";
static uint8_t nor_mem[NOR_SIZE];
static int nor_sync(uint32_t a, uint32_t n) {
    FILE *f = fopen(nor_path, "r+b"); if (!f) f = fopen(nor_path, "w+b");
    if (!f) return -1;
    fseek(f, (long)a, SEEK_SET); size_t w = fwrite(nor_mem + a, 1, n, f); fclose(f);
    return w == n ? 0 : -1;
}
static int n_read(nor_dev_t *d, uint32_t a, void *b, uint32_t n) { (void)d; if (a + n > NOR_SIZE) return -1; memcpy(b, nor_mem + a, n); return 0; }
static int n_prog(nor_dev_t *d, uint32_t a, const void *b, uint32_t n) {
    (void)d; if (a + n > NOR_SIZE) return -1;
    for (uint32_t i = 0; i < n; ++i) nor_mem[a + i] &= ((const uint8_t *)b)[i];
    return nor_sync(a, n);
}
static int n_erase(nor_dev_t *d, uint32_t a) {
    (void)d; a -= a % 4096u; if (a >= NOR_SIZE) return -1;
    memset(nor_mem + a, 0xFF, 4096u);
    return nor_sync(a, 4096u);
}
static nor_dev_t nor_dev = { n_read, n_prog, n_erase, NOR_SIZE, 4096u, 256u, true, NULL };
/* program flash: two 1 MB banks + the swap flag, in a file */
#include "fw_flash_sim.h"
#define FW_BANK (1024u * 1024u)
static char fw_path[512];
static uint8_t fw_banks[2][FW_BANK];
static fw_flash_sim_t fw_sim;
static fw_flash_t fw_dev;
static void fw_file_save(void) {
    FILE *f = fopen(fw_path, "wb");
    if (!f) return;
    fwrite(fw_banks, 1, sizeof fw_banks, f);
    uint8_t sw = fw_sim.swapped ? 1 : 0;
    fwrite(&sw, 1, 1, f);
    fclose(f);
}
static int (*sim_erase)(fw_flash_t *, uint32_t);
static int (*sim_write)(fw_flash_t *, uint32_t, const void *, uint32_t);
static int fw_erase_f(fw_flash_t *f, uint32_t off) { int r = sim_erase(f, off); fw_file_save(); return r; }
static int fw_write_f(fw_flash_t *f, uint32_t off, const void *p, uint32_t n) { int r = sim_write(f, off, p, n); if (n >= 256u || off + n >= FW_BANK - 8192u) fw_file_save(); return r; }
static void fw_swap_f(fw_flash_t *f) {
    (void)f;
    fw_sim.swapped = !fw_sim.swapped;
    fw_file_save();
    fprintf(stderr, "netsim: banks swapped, resetting (exit 3)\n");
    _exit(3);
}
struct fw_flash *bsp_fw_flash(void) {
    fw_flash_sim_init(&fw_dev, &fw_sim, fw_banks[0], fw_banks[1], FW_BANK);
    FILE *f = fopen(fw_path, "rb");
    if (f) {
        size_t n = fread(fw_banks, 1, sizeof fw_banks, f);
        uint8_t sw = 0;
        if (n == sizeof fw_banks && fread(&sw, 1, 1, f) == 1) fw_sim.swapped = sw != 0;
        fclose(f);
    } else {
        memset(fw_banks, 0xFF, sizeof fw_banks);
    }
    sim_erase = fw_dev.erase; sim_write = fw_dev.write;
    fw_dev.erase = fw_erase_f; fw_dev.write = fw_write_f; fw_dev.swap_and_reset = fw_swap_f;
    return &fw_dev;
}

nor_dev_t *bsp_log_nor(void) {
    memset(nor_mem, 0xFF, sizeof nor_mem);
    FILE *f = fopen(nor_path, "rb");
    if (f) { size_t n = fread(nor_mem, 1, sizeof nor_mem, f); fclose(f); fprintf(stderr, "NOR image %s: %zu bytes\n", nor_path, n); }
    else nor_sync(0, NOR_SIZE);
    return &nor_dev;
}
nvm_dev_t *bsp_param_nvm(void) {
    FILE *f = fopen(fram_path, "rb");
    if (f) { size_t n = fread(fram, 1, sizeof fram, f); fclose(f); fprintf(stderr, "FRAM image %s: %zu bytes\n", fram_path, n); }
    return &fram_dev;
}

/* ---------------- tasks ---------------- */
scan_engine_t plat_scan;
plat_cmdq_t plat_cmdq;
plat_seqlock_t plat_snap_lock;
static mbtcp_server_t mbs;
static http_server_t hs, hr;           /* HTTPS, and the plain-HTTP redirect on the next port */
static uint8_t redirect_buf[2048];
static uint8_t http_buf[4096 + STORE_FAC_SLOT + STORE_USR_SLOT + 64];
static volatile int running = 1;

static void sleep_until(struct timespec *t, long ms) {
    t->tv_nsec += ms * 1000000L;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, t, NULL);
}
static void *scan_thread(void *a) {
    (void)a; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    while (running) { sleep_until(&t, 10); sim_di = ((bsp_ms() / 1000u) % 12u) < 8u ? 1u : 0u; scan_step(&plat_scan); }
    return NULL;
}
static void *store_thread(void *a) {
    (void)a; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    uint32_t fw_t = bsp_ms(), fw_scans = 0;
    while (running) {
        sleep_until(&t, 20); param_store_poll(bsp_ms()); plat_log_poll(bsp_ms());
        uint32_t now = bsp_ms();
        if (now - fw_t >= 1000u) { uint32_t sc = scan_stats(&plat_scan).scans; fw_service(now, sc - fw_scans >= 50u); fw_scans = sc; fw_t = now; }
    }
    return NULL;
}
static void *iobus_thread(void *a) {
    (void)a; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    while (running) {
        sleep_until(&t, PLAT_IOB_PERIOD_MS);
        plat_host_crit_enter();
        simnode[0].inputs = (uint8_t)(((bsp_ms() / 1000u) % 12u) < 8u ? 0x01u : 0x00u) | (simnode[0].inputs & 0xFEu);
        plat_host_crit_exit();
        iob_cycle(bsp_ms());
    }
    return NULL;
}
/* console: node <n> <what> [value] */
static bool node_cmd(const char *line) {
    unsigned k; char what[16]; unsigned v = 0;
    int got = sscanf(line, "node %u %15s %i", &k, what, (int *)&v);
    if (got < 2 || k < 1 || k > 2) return false;
    iob_node_t *n = &simnode[k - 1];
    plat_host_crit_enter();
    if (!strcmp(what, "power")) n->powered = v != 0;
    else if (!strcmp(what, "restart")) iob_node_restart(n, false, bsp_ms());
    else if (!strcmp(what, "wdg")) iob_node_restart(n, true, bsp_ms());
    else if (!strcmp(what, "in")) n->inputs = (uint8_t)v;
    else if (!strcmp(what, "open")) n->a_open = (uint8_t)v;
    else if (!strcmp(what, "short")) n->a_overload = (uint8_t)v;
    else if (!strcmp(what, "hot")) n->a_overtemp = (uint8_t)v;
    else if (!strcmp(what, "wire")) n->wirebreak = (uint8_t)v;
    else if (!strcmp(what, "bfault")) n->b_fault = v ? IOB_BF_FAULT : 0;
    else if (!strcmp(what, "fielda")) n->field_a = v != 0;
    else if (!strcmp(what, "fieldb")) n->field_b = v != 0;
    else if (!strcmp(what, "addr") && v <= IOB_ADDR_MAX) n->address = (uint8_t)v;
    else if (!strcmp(what, "new")) { n->address = 0; n->ip = n->mask = n->gw = n->master_ip = 0; }   /* a new board: no address, no IP */
    else { plat_host_crit_exit(); return false; }
    plat_host_crit_exit();
    fprintf(stderr, "I/O board %u: %s %u\n", k, what, v);
    return true;
}
static void *modbus_thread(void *a) { (void)a; while (running) mbtcp_poll(&mbs, 100); return NULL; }
static void *sntp_thread(void *a) {
    (void)a;
    while (running) { struct timespec t = { 0, 200000000L }; nanosleep(&t, NULL); sntp_service(bsp_ms()); }
    return NULL;
}
static void release_web(void) { http_drop_kept(&hs); }
static void *http_thread(void *a)   { (void)a; while (running) { http_poll(&hs, 50); http_poll(&hr, 50); mail_service(bsp_ms(), NULL, release_web); } return NULL; }
/* stand-in for the touchscreen: "remote", "local", "set <id> <value>" on stdin */
static void *console_thread(void *a) {
    (void)a;
    char line[128];
    /* read(2), not fgets: a thread blocked inside stdio holds the stdin lock, and
     * exit() would then wait for it for ever while flushing the streams */
    for (;;) {
        size_t n = 0;
        char ch;
        while (n + 1 < sizeof line && read(0, &ch, 1) == 1 && ch != '\n') line[n++] = ch;
        if (n == 0 && !running) break;
        line[n] = 0;
        if (n == 0) { struct timespec t = { 0, 50000000L }; nanosleep(&t, NULL); continue; }
        unsigned id; long v;
        param_err_t e = PERR_OK;
        plat_val_t val;
        if (!strncmp(line, "remote", 6)) { val.u = 1; e = param_post(PARAM_ID_CTRL_MODE, PLAT_VAL_U32, val, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR); }
        else if (!strncmp(line, "fwyes", 5)) { fw_local_answer(true, 0); continue; }   /* the touchscreen's answer */
        else if (!strncmp(line, "fwno", 4)) { fw_local_answer(false, 0); continue; }
        else if (!strncmp(line, "node ", 5)) { if (!node_cmd(line)) fprintf(stderr, "node: ?\n"); continue; }
        else if (!strncmp(line, "local", 5)) { val.u = 0; e = param_post(PARAM_ID_CTRL_MODE, PLAT_VAL_U32, val, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR); }
        else if (sscanf(line, "set %u %ld", &id, &v) == 2) { val.i = (int32_t)v; e = param_post((uint16_t)id, PLAT_VAL_I32, val, PLAT_SRC_LOCAL, PLAT_LVL_FACTORY); }
        else continue;
        fprintf(stderr, "touchscreen: %s", e == PERR_OK ? "accepted\n" : "refused\n");
    }
    return NULL;
}
static void http_wait(void) { struct timespec t = { 0, 10000000L }; nanosleep(&t, NULL); }
static void on_signal(int s) { (void)s; running = 0; }

int main(int argc, char **argv) {
    uint16_t mport = argc > 1 ? (uint16_t)atoi(argv[1]) : 1502, hport = argc > 2 ? (uint16_t)atoi(argv[2]) : 8443;
    if (argc > 3) fram_path = argv[3];
    if (argc > 4) nor_path = argv[4];
    snprintf(fw_path, sizeof fw_path, "%.*sfw.bin", (int)(strlen(nor_path) > 7 ? strlen(nor_path) - 7 : 0), nor_path);
    if (getenv("CEMS_NTP_PORT")) sntp_port = (uint16_t)atoi(getenv("CEMS_NTP_PORT"));
    if (getenv("CEMS_MAIL_RETRY_MS")) mail_retry_ms = (uint32_t)atoi(getenv("CEMS_MAIL_RETRY_MS"));
    if (getenv("CEMS_FW_TRIAL_MS")) fw_trial_ms = (uint32_t)atoi(getenv("CEMS_FW_TRIAL_MS"));
    const char *fwver = getenv("CEMS_FW_VERSION");
    static uint8_t fwkey[65];                    /* CEMS_FW_PUBKEY: hex public key, as a signed production build */
    const char *kx = getenv("CEMS_FW_PUBKEY");
    if (kx && strlen(kx) == 130) {
        for (int k = 0; k < 65; ++k) { unsigned v; sscanf(kx + 2 * k, "%2x", &v); fwkey[k] = (uint8_t)v; }
        fw_pubkey_override_for_test(fwkey);
    }
    static uint8_t devkey[65];                   /* CEMS_DEV_PUBKEY: hex developer key (serial tokens, D25) */
    const char *dx = getenv("CEMS_DEV_PUBKEY");
    if (dx && strlen(dx) == 130) {
        for (int k = 0; k < 65; ++k) { unsigned v; sscanf(dx + 2 * k, "%2x", &v); devkey[k] = (uint8_t)v; }
        plat_serial_key_override_for_test(devkey);
    }
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma); pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&crit, &ma);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    /* the same start-up order as plat_start() */
    const plat_app_t *app = app_select();     /* whichever application is linked in (make netsim APP=...) */
    plat_diag_boot(bsp_reset_cause());
    plat_cmdq_init(&plat_cmdq);
    scan_init(&plat_scan, app->scan, &scan_hw_bsp, &plat_cmdq, &plat_snap_lock, 10);
    if (param_init(app->params, app->n_params) != PERR_OK) { fprintf(stderr, "param table error at id %u\n", param_table_error()); return 1; }
    { uint16_t ub = ui_app_check(app->ui); if (ub) { fprintf(stderr, "screen/record description error at id %u\n", ub); return 1; } }
    ui_app_set(app->ui);
    if (!param_store_init(bsp_param_nvm(), NULL, app->retain, app->n_retain, app->layout_version, app->migrate)) { fprintf(stderr, "store table error\n"); return 1; }
    param_store_boot();
    {
        bsp_rtc_t rt; bsp_rtc_read(&rt);
        plat_log_cfg_t lc = { .nor = bsp_log_nor(), .fram = &fram_dev, .fram_addr = STORE_META_ADDR,
                              .chans = app->log_chans, .n_chans = (unsigned)app->n_log_chans,
                              .fw_version = fwver ? fwver : app->ident.revision, .reset_cause = (uint8_t)plat_diag.last_reset_cause,
                              .unix_now = plat_unix_from_civil(rt.year, rt.month, rt.day, rt.hours, rt.minutes, rt.seconds) };
        plat_log_init(&lc, bsp_ms());
        fw_init(bsp_fw_flash(), &fram_dev, STORE_META_ADDR + 128u, fwver ? fwver : app->ident.revision);
        fw_boot((uint8_t)plat_diag.last_reset_cause, bsp_ms());   /* may "reset" (exit 3) to roll back */
        fw_safe_check = app->fw_safe;
    }
    { int ae = alarm_init(app->alarms, app->n_alarms, (uint8_t)plat_diag.last_reset_cause);
      if (ae) { fprintf(stderr, "alarm table error at id %d\n", ae); return 1; } }
    { int me = plat_state_init(app->meas, app->n_meas);
      if (me) { fprintf(stderr, "measurement table error at entry %d\n", me - 1); return 1; } }
    param_bind(&plat_cmdq, &plat_snap_lock);
    scan_set_sys(&plat_scan, &plat_sys);
    iob_simbus_init(&simbus);
    simbus.now = bsp_ms;
    for (unsigned k = 0; k < 2; ++k) { iob_node_init(&simnode[k], (uint8_t)(k + 1)); iob_simbus_add(&simbus, &simnode[k]); }
    iob_init(bsp_rs485());
    eth_init();
    auth_init();
    int bad = mb_init(app->modbus_map, app->n_modbus_map, &app->ident);
    if (bad) { fprintf(stderr, "modbus map error at entry %d\n", bad - 1); return 1; }
    http_cfg_t hc = { .product = app->ident.product, .fw_version = fwver ? fwver : app->ident.revision, .scan = &plat_scan,
                      .scratch = http_buf, .scratch_size = sizeof http_buf, .wait = http_wait,
                      .log_chans = app->log_chans, .n_log_chans = (unsigned)app->n_log_chans };
    http_api_init(&hc);
    if (mbtcp_open(&mbs, mport) != 0) { perror("modbus listen"); return 1; }
    {
        int trc = tls_init(&fram_dev, STORE_META_ADDR + 256u, param_tag_get(param_find(PARAM_ID_SERIAL)).u,
                           0x7F000001u);                  /* certificate for 127.0.0.1 */
        if (trc != 0) { fprintf(stderr, "tls_init failed: -0x%04x\n", (unsigned)-trc); return 1; }
        if (tls_key_was_created()) plat_log_event(LOG_EV_TLS_KEY, PLAT_SRC_INTERNAL, 0, 0, NAN, NAN, NULL);
        fprintf(stderr, "HTTPS certificate SHA-256 %s\n", tls_fingerprint());
    }
    if (http_open(&hs, hport, http_buf, sizeof http_buf) != 0) { perror("https listen"); return 1; }
    hs.tls = true;
    if (http_open(&hr, (uint16_t)(hport + 1), redirect_buf, sizeof redirect_buf) != 0) { perror("http listen"); return 1; }
    hr.redirect_only = true;
    fprintf(stderr, "netsim: Modbus TCP on %u, web on https://127.0.0.1:%u/ (http on the next port redirects), store status 0x%04x\n",
            mport, hport, param_store_status());

    pthread_t th[7];
    pthread_create(&th[4], NULL, console_thread, NULL);
    pthread_detach(th[4]);
    pthread_create(&th[0], NULL, scan_thread, NULL);
    pthread_create(&th[1], NULL, store_thread, NULL);
    pthread_create(&th[2], NULL, modbus_thread, NULL);
    pthread_create(&th[3], NULL, http_thread, NULL);
    pthread_create(&th[5], NULL, sntp_thread, NULL);
    pthread_create(&th[6], NULL, iobus_thread, NULL);
    for (int k = 0; k < 4; ++k) pthread_join(th[k], NULL);
    pthread_join(th[5], NULL);
    pthread_join(th[6], NULL);
    param_store_poll(bsp_ms() + 5000); param_store_scan_service(); param_store_poll(bsp_ms() + 5000);   /* last save */
    plat_log_poll(bsp_ms());
    scan_stats_t ss = scan_stats(&plat_scan);
    fprintf(stderr, "netsim: stopped. scans %u, late %u, max dt %u ms; modbus frames %u; http served %u\n",
            ss.scans, ss.late_starts, ss.max_dt_ms, mbs.frames, hs.served);
    mbtcp_close_all(&mbs); http_close(&hs); http_close(&hr);
    return 0;
}

/* the chip's unique ID: CEMS_UID (24 hex digits) or a fixed stand-in */
void bsp_chip_uid(uint8_t uid[12]) {
    static const uint8_t dflt[12] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb };
    const char *e = getenv("CEMS_UID");
    for (int k = 0; k < 12; ++k) {
        unsigned b;
        uid[k] = e && strlen(e) == 24 && sscanf(e + 2 * k, "%2x", &b) == 1 ? (uint8_t)b : dflt[k];
    }
}
