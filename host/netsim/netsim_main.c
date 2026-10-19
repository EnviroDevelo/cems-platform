/* netsim_main.c - the platform on a PC with real sockets and real time:
 * scan (10 ms), store (20 ms), Modbus TCP and web server, each in its own thread
 * as on the RTOS. The parameter FRAM is a file, so settings survive restarts.
 *
 *   ./build/netsim [modbus_port] [http_port] [fram_file]      (defaults 1502 8080 netsim_fram.bin)
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
#include "web_auth.h"
#include "plat_diag.h"
#include "plat_tasks.h"
#include "app_demo.h"

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
bool bsp_rtc_read(bsp_rtc_t *r) {
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    *r = (bsp_rtc_t){ (uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday,
                      (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec, (uint8_t)(tm.tm_wday == 0 ? 7 : tm.tm_wday) };
    return true;
}
void bsp_io_read(plat_io_in_t *in) { in->di = sim_di; }
void bsp_io_write(const plat_io_out_t *out) { sim_out = *out; }
void bsp_io_safe_state(void) { memset(&sim_out, 0, sizeof sim_out); }
void bsp_wdg_kick(void) { }
bsp_reset_cause_t bsp_reset_cause(void) { return BSP_RESET_POWER_ON; }
void bsp_system_reset(void) { fprintf(stderr, "reset requested\n"); exit(3); }
void bsp_random(void *buf, uint32_t len) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, len, f) != len) abort();
    fclose(f);
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
static http_server_t hs;
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
    while (running) { sleep_until(&t, 20); param_store_poll(bsp_ms()); }
    return NULL;
}
static void *modbus_thread(void *a) { (void)a; while (running) mbtcp_poll(&mbs, 100); return NULL; }
static void *http_thread(void *a)   { (void)a; while (running) http_poll(&hs, 100); return NULL; }
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
    uint16_t mport = argc > 1 ? (uint16_t)atoi(argv[1]) : 1502, hport = argc > 2 ? (uint16_t)atoi(argv[2]) : 8080;
    if (argc > 3) fram_path = argv[3];
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma); pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&crit, &ma);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal); signal(SIGPIPE, SIG_IGN);

    /* the same start-up order as plat_start() */
    const plat_app_t *app = &app_demo_plat;
    plat_diag_boot(bsp_reset_cause());
    plat_cmdq_init(&plat_cmdq);
    scan_init(&plat_scan, app->scan, &scan_hw_bsp, &plat_cmdq, &plat_snap_lock, 10);
    if (param_init(app->params, app->n_params) != PERR_OK) { fprintf(stderr, "param table error at id %u\n", param_table_error()); return 1; }
    if (!param_store_init(bsp_param_nvm(), NULL, app->retain, app->n_retain, app->layout_version, app->migrate)) { fprintf(stderr, "store table error\n"); return 1; }
    param_store_boot();
    param_bind(&plat_cmdq, &plat_snap_lock);
    scan_set_sys(&plat_scan, &plat_sys);
    auth_init();
    int bad = mb_init(app->modbus_map, app->n_modbus_map, &app->ident);
    if (bad) { fprintf(stderr, "modbus map error at entry %d\n", bad - 1); return 1; }
    http_cfg_t hc = { .product = app->ident.product, .fw_version = app->ident.revision, .scan = &plat_scan,
                      .scratch = http_buf, .scratch_size = sizeof http_buf, .wait = http_wait };
    http_api_init(&hc);
    if (mbtcp_open(&mbs, mport) != 0) { perror("modbus listen"); return 1; }
    if (http_open(&hs, hport, http_buf, sizeof http_buf) != 0) { perror("http listen"); return 1; }
    fprintf(stderr, "netsim: Modbus TCP on %u, web on http://127.0.0.1:%u/, store status 0x%04x\n",
            mport, hport, param_store_status());

    pthread_t th[5];
    pthread_create(&th[4], NULL, console_thread, NULL);
    pthread_detach(th[4]);
    pthread_create(&th[0], NULL, scan_thread, NULL);
    pthread_create(&th[1], NULL, store_thread, NULL);
    pthread_create(&th[2], NULL, modbus_thread, NULL);
    pthread_create(&th[3], NULL, http_thread, NULL);
    for (int k = 0; k < 4; ++k) pthread_join(th[k], NULL);
    param_store_poll(bsp_ms() + 5000); param_store_scan_service(); param_store_poll(bsp_ms() + 5000);   /* last save */
    scan_stats_t ss = scan_stats(&plat_scan);
    fprintf(stderr, "netsim: stopped. scans %u, late %u, max dt %u ms; modbus frames %u; http served %u\n",
            ss.scans, ss.late_starts, ss.max_dt_ms, mbs.frames, hs.served);
    mbtcp_close_all(&mbs); http_close(&hs);
    return 0;
}
