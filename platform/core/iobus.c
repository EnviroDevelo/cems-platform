/* iobus.c - I/O bus master (iobus.h), over RS-485 or Ethernet (iob_net.h). Pure C: the port is passed in, so the
 * host tests run it against a simulated node (platform/bsp/iob_node_sim.c). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "iobus.h"
#include "mb_rtu.h"
#include "bsp.h"
#include "plat_alarm.h"
#include "plat_cmdq.h"
#include "plat_config.h"
#include "plat_log.h"
#include "plat_port.h"

/* ---------------- settings (parameter tags, owned by the scan) ---------------- */
iob_board_tags_t iob_tags[IOB_BOARDS] = {
    { .address = 1, .first_do = 1,  .first_di = 1,  .ol_mask = 0xFF, .ip = 0xC0A8013Du, .first_temp = 5,  .pwm_hz = 1000, .tach_ppr = 2 },
    { .address = 2, .first_do = 19, .first_di = 9,  .ol_mask = 0xFF, .ip = 0xC0A8013Eu, .first_temp = 11, .pwm_hz = 1000, .tach_ppr = 2 },
    { .address = 3, .first_do = 37, .first_di = 17, .ol_mask = 0xFF, .ip = 0xC0A8013Fu, .first_temp = 17, .pwm_hz = 1000, .tach_ppr = 2 },
};
uint16_t iob_hold_s = 300;
uint8_t  iob_filter_ms = 8;
uint8_t  iob_link = IOB_LINK_IOPORT;   /* the product's separate I/O network (HARDWARE.md H5) */
uint32_t iob_port_ip = 0xC0A8013Cu;      /* 192.168.1.60: beside the boards' default IPs, on its own network */
uint8_t  iob_port_prefix = 24;
uint8_t  iob_port_status;

typedef struct {
    bool en; uint8_t addr, first_do, first_di, ol, wb; uint32_t ip;
    uint16_t temp[IOB_R_CONFIG2_N];      /* map 2: registers 96..106 as they are written */
} cfg_t;

/* scan -> bus (under PLAT_CRIT) */
static struct {
    cfg_t    cfg[IOB_BOARDS];
    uint16_t hold_s;
    uint8_t  filter_ms;
    uint8_t  link;                      /* IOB_LINK_* */
    bool     cfg_valid;                 /* the scan has copied the settings at least once */
    uint16_t out[IOB_BOARDS];
    uint8_t  rel[IOB_BOARDS];
    uint16_t pwm[IOB_BOARDS];           /* duty 0..1000 */
    uint8_t  dir[IOB_BOARDS];
    uint32_t stamp;                     /* bsp ms when the scan last wrote outputs */
    bool     out_valid;
} cmd;

/* bus -> everyone (under PLAT_CRIT) */
static iob_view_t view[IOB_BOARDS];
static uint32_t   wdg_events[IOB_BOARDS];   /* node watchdog resets seen (the scan raises the alarm) */

/* commissioning (iob_cm_*) */
static struct { uint8_t op, from, to; } cm_req;        /* any task -> bus (under PLAT_CRIT); op 0 = none */
static iob_cm_view_t cm_view;                          /* bus -> any task (under PLAT_CRIT) */

/* Ethernet commissioning (iob_dc_*) */
static struct { uint8_t op; iob_disc_t set; } dc_req;  /* any task -> bus (under PLAT_CRIT); op 0 = none */
static iob_dc_view_t dc_view;                          /* bus -> any task (under PLAT_CRIT) */

static volatile bool safe;
static iob_port_t   *port;                             /* RS-485 */
static iob_port_t   *tcp;                              /* Ethernet: Modbus TCP */
static iob_dgram_t  *disc;                             /* Ethernet: discovery */
static iob_port_t   *io_tcp;                           /* the I/O port (W5500): Modbus TCP */
static iob_dgram_t  *io_disc;                          /* the I/O port: discovery */
static iob_port_t   *bus;                              /* the link in use this cycle */
static uint8_t       bus_link = 0xFF;

/* bus task private */
typedef struct {
    uint8_t  state;
    uint32_t next_try, last_ok;
    uint16_t seq;
    bool     cfg_sent;
    cfg_t    sent;
    uint16_t sent_timeout;
    uint8_t  sent_filter;
    bool     ident_ok;
    bool     answered;                  /* replied at least once since power-up */
    uint8_t  map, temps, pwm;           /* the board's map version and what it has (map 2) */
    uint32_t temp_at;                   /* the temperatures are read next at */
    bool     temp_cfg_sent;
    uint16_t temp_sent[IOB_R_CONFIG2_N];
    /* last raw values, for the event log */
    uint16_t st;
    uint8_t  a_open, a_ovl, a_otemp, wb, bf;
    bool     have_raw;
} bstate_t;
static bstate_t B[IOB_BOARDS];

void iob_init(iob_port_t *p) { port = p; }
void iob_init_eth(iob_port_t *t, iob_dgram_t *d) { tcp = t; disc = d; }
void iob_init_ioport(iob_port_t *t, iob_dgram_t *d) { io_tcp = t; io_disc = d; }

/* a link with no port on this board (the CORE PCB has no RS-485) cannot be chosen */
int iob_link_interlock(uint16_t id, plat_val_t v) {
    (void)id;
    iob_port_t *p = v.u == IOB_LINK_IOPORT ? io_tcp : v.u == IOB_LINK_ETH ? tcp : port;
    return p ? 0 : 1;
}

void iob_reset(void) {
    PLAT_CRIT_ENTER();
    memset(&cmd, 0, sizeof cmd);
    memset(view, 0, sizeof view);
    memset(wdg_events, 0, sizeof wdg_events);
    memset(&cm_req, 0, sizeof cm_req);
    memset(&cm_view, 0, sizeof cm_view);
    memset(&dc_req, 0, sizeof dc_req);
    memset(&dc_view, 0, sizeof dc_view);
    PLAT_CRIT_EXIT();
    memset(B, 0, sizeof B);
    safe = false;
    bus_link = 0xFF;
}

void iob_safe_state(void) { safe = true; }

const char *iob_state_name(uint8_t s) {
    static const char *const n[] = { "off", "connecting", "online", "not responding", "wrong type", "settings conflict" };
    return s <= IOB_S_BAD_SETTINGS ? n[s] : "?";
}

int iob_fault_lines(const iob_view_t *v, uint8_t ol, uint8_t wbm, char lines[][IOB_LINE], int max) {
    int n = 0;
    if (max <= 0) return 0;
    switch (v->state) {
    case IOB_S_LOST:         snprintf(lines[0], IOB_LINE, "No reply: board switches off after hold"); return 1;
    case IOB_S_WRONG_TYPE:   snprintf(lines[0], IOB_LINE, "Not a CEMS I/O board"); return 1;
    case IOB_S_BAD_SETTINGS: snprintf(lines[0], IOB_LINE, "Channels or address overlap"); return 1;
    case IOB_S_RUN: break;
    default: return 0;
    }
    if ((v->status & IOBS_COMMS_LATCHED) && n < max) snprintf(lines[n++], IOB_LINE, "Link was lost: acknowledge");
    for (unsigned i = 0; i < 8 && n < max; ++i) {
        uint8_t m = (uint8_t)(1u << i);
        const char *k = (v->a_overload & m) ? "short or overload" : (v->a_overtemp & m) ? "overheating" :
                        (v->a_open & ol & m) ? "open circuit" : NULL;
        if (k) snprintf(lines[n++], IOB_LINE, "Out %u: %s", i + 1u, k);
    }
    if ((v->b_fault & IOB_BF_FAULT) && n < max) snprintf(lines[n++], IOB_LINE, "Outs 9-16: overload or hot");
    if ((v->b_fault & IOB_BF_WDG) && n < max) snprintf(lines[n++], IOB_LINE, "Outs 9-16: off (own watchdog)");
    for (unsigned i = 0; i < 8 && n < max; ++i)
        if (v->wirebreak & wbm & (1u << i)) snprintf(lines[n++], IOB_LINE, "In %u: wire break", i + 1u);
    for (unsigned i = 0; i < IOB_TEMPS && n < max; ++i) {
        uint8_t t = v->temp_st[i];
        if (t != IOB_TS_OK && t != IOB_TS_OFF && t != IOB_TS_WAIT)
            snprintf(lines[n++], IOB_LINE, "Temp %u: %s", i + 1u, iob_temp_state_name(t));
    }
    if (!(v->status & IOBS_FIELD_A_OK) && n < max) snprintf(lines[n++], IOB_LINE, "Field 24 V A missing");
    if (!(v->status & IOBS_FIELD_B_OK) && n < max) snprintf(lines[n++], IOB_LINE, "Field 24 V B missing");
    return n;
}

const char *iob_temp_type_name(uint8_t t) {
    switch (t) {
    case IOB_TT_NONE: return "none";
    case IOB_TT_TC_K: return "K"; case IOB_TT_TC_J: return "J"; case IOB_TT_TC_T: return "T";
    case IOB_TT_TC_E: return "E"; case IOB_TT_TC_S: return "S";
    case IOB_TT_PT100_2W: return "PT100 2-wire"; case IOB_TT_PT100_3W: return "PT100 3-wire"; case IOB_TT_PT100_4W: return "PT100 4-wire";
    case IOB_TT_PT1000_2W: return "PT1000 2-wire"; case IOB_TT_PT1000_3W: return "PT1000 3-wire"; case IOB_TT_PT1000_4W: return "PT1000 4-wire";
    case IOB_TT_NTC: return "NTC";
    default: return "?";
    }
}

const char *iob_temp_state_name(uint8_t st) {
    static const char *const n[] = { "ok", "not used", "starting", "open circuit", "short circuit", "below range",
                                     "above range", "converter fault", "no cold junction" };
    return st <= IOB_TS_CJ ? n[st] : "?";
}

int32_t iob_temp_cal(int16_t raw, int16_t off, int16_t gain) {
    int64_t cd = (int64_t)raw * 10;                               /* 0.01 degC */
    cd += (cd * gain + (cd >= 0 ? 5000 : -5000)) / 10000;         /* gain - 1 in 0.01 % */
    return (int32_t)(cd + off);
}

static bool tt_known(uint8_t t) { return strcmp(iob_temp_type_name(t), "?") != 0; }

bool iob_board_view(unsigned b, iob_view_t *v) {
    if (b >= IOB_BOARDS) return false;
    PLAT_CRIT_ENTER();
    *v = view[b];
    PLAT_CRIT_EXIT();
    return true;
}

/* Boards whose channel range does not fit the image or overlaps an earlier enabled
 * board's (bit k = board k). */
static bool ip_ok(uint32_t ip) {
    uint8_t top = (uint8_t)(ip >> 24), low = (uint8_t)ip;
    return top != 0 && top != 127 && top < 224 && low != 0 && low != 255;
}

static uint8_t bad_settings(const cfg_t *c, uint8_t link) {
    uint8_t bad = 0;
    for (unsigned k = 0; k < IOB_BOARDS; ++k) {
        if (!c[k].en) continue;
        if (IOB_LINK_IS_ETH(link) && !ip_ok(c[k].ip)) { bad |= (uint8_t)(1u << k); continue; }
        if (link == IOB_LINK_IOPORT && c[k].ip == iob_port_ip) { bad |= (uint8_t)(1u << k); continue; }   /* the I/O port's own */
        if (c[k].addr < 1 || c[k].addr > IOB_ADDR_MAX || c[k].first_do < 1 || c[k].first_di < 1 ||
            c[k].first_do - 1u + IOB_OUTS > PLAT_DO_MAX || c[k].first_di - 1u + IOB_INS > PLAT_DI_MAX) { bad |= (uint8_t)(1u << k); continue; }
        for (unsigned j = 0; j < k; ++j) {
            if (!c[j].en || (bad & (1u << j))) continue;
            bool o_do = c[k].first_do < c[j].first_do + IOB_OUTS && c[j].first_do < c[k].first_do + IOB_OUTS;
            bool o_di = c[k].first_di < c[j].first_di + IOB_INS && c[j].first_di < c[k].first_di + IOB_INS;
            bool same = IOB_LINK_IS_ETH(link) ? c[k].ip == c[j].ip : c[k].addr == c[j].addr;
            if (o_do || o_di || same) { bad |= (uint8_t)(1u << k); break; }
        }
    }
    return bad;
}

static void ev(unsigned b, const char *text) {
    plat_log_event(LOG_EV_IOB, PLAT_SRC_INTERNAL, 0, b + 1u, NAN, NAN, text);
}

/* ---------------- one request ---------------- */
static int transact(unsigned b, const uint8_t *tx, size_t n, uint16_t *regs, uint16_t n_regs) {
    uint8_t rx[MBR_FRAME_MAX];
    uint16_t got = 0;
    if (n == 0) return MBR_BAD_FORMAT;
    if (bus && bus->xfer) got = bus->xfer(bus, tx, (uint16_t)n, rx, sizeof rx, PLAT_IOB_REPLY_MS);
    uint8_t exc = 0;
    int r = mbr_check_reply(tx, n, rx, got, regs, n_regs, &exc);
    PLAT_CRIT_ENTER();
    view[b].requests++;
    if (r != MBR_OK) { view[b].errors++; view[b].last_error = (uint8_t)r; view[b].last_exception = exc; }
    PLAT_CRIT_EXIT();
    return r;
}

static void set_state(unsigned b, uint8_t s, uint32_t now) {
    bstate_t *x = &B[b];
    if (x->state == s) return;
    uint8_t was = x->state;
    x->state = s;
    switch (s) {
    case IOB_S_RUN:          ev(b, "online"); x->answered = true; break;
    case IOB_S_LOST:         ev(b, "not responding"); x->next_try = now + PLAT_IOB_RETRY_MS; break;
    case IOB_S_WRONG_TYPE:   ev(b, "wrong type"); x->next_try = now + PLAT_IOB_RETRY_MS; break;
    case IOB_S_BAD_SETTINGS: ev(b, "settings conflict"); break;
    case IOB_S_CONNECTING:   x->last_ok = now; x->ident_ok = false; x->cfg_sent = false; break;
    default: break;
    }
    if (s != IOB_S_RUN) x->have_raw = false;
    (void)was;
}

/* A request failed. The board stays as it is until nothing good has come for
 * PLAT_IOB_LOST_MS, then it is "not responding". */
static void failed(unsigned b, uint32_t now) {
    bstate_t *x = &B[b];
    if (x->state == IOB_S_LOST || x->state == IOB_S_WRONG_TYPE) { x->next_try = now + PLAT_IOB_RETRY_MS; return; }
    /* After power-up the network and the board need a few seconds: not "lost" (no alarm, outputs
     * stay off as in any state but online) until it has had PLAT_IOB_START_MS to answer once. */
    uint32_t lost_ms = x->answered ? PLAT_IOB_LOST_MS : PLAT_IOB_START_MS;
    if ((uint32_t)(now - x->last_ok) >= lost_ms) set_state(b, IOB_S_LOST, now);
}

static bool ident(unsigned b, uint8_t addr) {
    uint8_t f[MBR_FRAME_MAX];
    uint16_t r[IOB_R_IDENT_N];
    int rc = transact(b, f, mbr_req_read(f, addr, IOB_R_MAGIC, IOB_R_IDENT_N), r, IOB_R_IDENT_N);
    if (rc != MBR_OK) return false;
    uint16_t map = r[1], tp = map >= 2u ? r[IOB_R_N_TEMP_PWM - IOB_R_MAGIC] : 0u;
    B[b].map = (uint8_t)map;
    B[b].temps = (tp & 0xFFu) == IOB_TEMPS ? IOB_TEMPS : 0u;
    B[b].pwm = (tp >> 8) ? 1u : 0u;
    PLAT_CRIT_ENTER();
    view[b].hw_rev = r[IOB_R_HW_REV - IOB_R_MAGIC];
    view[b].fw_version = r[IOB_R_FW_VERSION - IOB_R_MAGIC];
    memcpy(view[b].uid, &r[IOB_R_UID - IOB_R_MAGIC], sizeof view[b].uid);
    view[b].map_version = (uint8_t)map;
    view[b].n_temps = B[b].temps;
    view[b].n_pwm = B[b].pwm;
    PLAT_CRIT_EXIT();
    bool match = r[0] == IOB_MAGIC && map >= 1u && map <= IOB_MAP_VERSION &&
                 r[IOB_R_N_OUT_A - IOB_R_MAGIC] == 8 && r[IOB_R_N_OUT_B - IOB_R_MAGIC] == 8 &&
                 r[IOB_R_N_RELAYS - IOB_R_MAGIC] == 2 && r[IOB_R_N_INPUTS - IOB_R_MAGIC] == 8;
    B[b].ident_ok = match;
    return true;
}

static bool write_cfg(unsigned b, const cfg_t *c, uint16_t timeout, uint8_t filter) {
    uint8_t f[MBR_FRAME_MAX];
    uint16_t v[IOB_R_CONFIG_N] = { (uint16_t)(timeout * 10u), filter, c->wb, c->ol };
    if (transact(b, f, mbr_req_write(f, c->addr, IOB_R_HOLD_DS, v, IOB_R_CONFIG_N), NULL, 0) != MBR_OK) return false;
    B[b].cfg_sent = true;
    B[b].sent = *c;
    B[b].sent_timeout = timeout;
    B[b].sent_filter = filter;
    return true;
}

static bool cfg_changed(const bstate_t *x, const cfg_t *c, uint16_t timeout, uint8_t filter) {
    return !x->cfg_sent || x->sent.ol != c->ol || x->sent.wb != c->wb || x->sent.addr != c->addr ||
           x->sent_timeout != timeout || x->sent_filter != filter;
}

/* map 2 settings: the temperature part (96..104) to a board with temperatures, the PWM part
 * (105..106) to one with a PWM output */
static bool temp_cfg_changed(const bstate_t *x, const cfg_t *c) {
    return (x->temps || x->pwm) && (!x->temp_cfg_sent || memcmp(x->temp_sent, c->temp, sizeof c->temp) != 0);
}

static bool write_cfg2(unsigned b, const cfg_t *c) {
    uint8_t f[MBR_FRAME_MAX];
    bstate_t *x = &B[b];
    const uint16_t nt = IOB_R_PWM_FREQ - IOB_R_TEMP_TYPE;
    if (x->temps && transact(b, f, mbr_req_write(f, c->addr, IOB_R_TEMP_TYPE, c->temp, nt), NULL, 0) != MBR_OK) return false;
    if (x->pwm && transact(b, f, mbr_req_write(f, c->addr, IOB_R_PWM_FREQ, c->temp + nt, IOB_R_CONFIG2_N - nt), NULL, 0) != MBR_OK)
        return false;
    x->temp_cfg_sent = true;
    memcpy(x->temp_sent, c->temp, sizeof c->temp);
    return true;
}

/* a sensor is configured on one of the board's connectors (else its temperatures are not read) */
static bool any_sensor(const cfg_t *c) {
    for (unsigned k = 0; k < IOB_TEMP_CONNS; ++k) if (c->temp[k] != IOB_TT_NONE) return true;
    return false;
}

/* map 2: the temperature block, every PLAT_IOB_TEMP_MS */
static bool read_temps(unsigned b, const cfg_t *c, uint32_t now) {
    uint8_t f[MBR_FRAME_MAX];
    uint16_t r[IOB_R_TEMPS_N];
    if (transact(b, f, mbr_req_read(f, c->addr, IOB_R_TEMP, IOB_R_TEMPS_N), r, IOB_R_TEMPS_N) != MBR_OK) return false;
    B[b].temp_at = now + PLAT_IOB_TEMP_MS;
    PLAT_CRIT_ENTER();
    for (unsigned i = 0; i < IOB_TEMPS; ++i) {
        view[b].temp[i] = (int16_t)r[i];
        view[b].temp_st[i] = (uint8_t)r[IOB_R_TEMP_STATE - IOB_R_TEMP + i];
    }
    view[b].cj = (int16_t)r[IOB_R_CJ_TEMP - IOB_R_TEMP];
    PLAT_CRIT_EXIT();
    return true;
}

/* event-log lines for what changed in the status block */
static void log_changes(unsigned b, uint16_t st, uint8_t open, uint8_t ovl, uint8_t otemp, uint8_t wb, uint8_t bf, const cfg_t *c) {
    bstate_t *x = &B[b];
    char t[24];
    uint16_t pst = x->have_raw ? x->st : (IOB_ST_FIELD_A_OK | IOB_ST_FIELD_B_OK);
    uint8_t pa = x->have_raw ? (uint8_t)((x->a_open & c->ol) | x->a_ovl | x->a_otemp) : 0;
    uint8_t na = (uint8_t)((open & c->ol) | ovl | otemp);
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t m = (uint8_t)(1u << i);
        if ((na & m) && !(pa & m)) {
            snprintf(t, sizeof t, "out %u %s", i + 1u, (ovl & m) ? "short/overload" : (otemp & m) ? "overheating" : "open circuit");
            ev(b, t);
        } else if (!(na & m) && (pa & m)) { snprintf(t, sizeof t, "out %u ok", i + 1u); ev(b, t); }
        uint8_t pw = x->have_raw ? (uint8_t)(x->wb & c->wb) : 0, nw = (uint8_t)(wb & c->wb);
        if ((nw & m) && !(pw & m)) { snprintf(t, sizeof t, "in %u wire break", i + 1u); ev(b, t); }
        else if (!(nw & m) && (pw & m)) { snprintf(t, sizeof t, "in %u ok", i + 1u); ev(b, t); }
    }
    uint8_t pb = x->have_raw ? x->bf : 0;
    if ((bf & IOB_BF_FAULT) && !(pb & IOB_BF_FAULT)) ev(b, "outs 9-16 fault");
    else if (!(bf & IOB_BF_FAULT) && (pb & IOB_BF_FAULT)) ev(b, "outs 9-16 ok");
    if ((bf & IOB_BF_WDG) && !(pb & IOB_BF_WDG)) ev(b, "outs 9-16 watchdog off");
    uint16_t chg = (uint16_t)(st ^ pst);
    if (chg & IOB_ST_FIELD_A_OK) ev(b, (st & IOB_ST_FIELD_A_OK) ? "field A 24V ok" : "field A 24V missing");
    if (chg & IOB_ST_FIELD_B_OK) ev(b, (st & IOB_ST_FIELD_B_OK) ? "field B 24V ok" : "field B 24V missing");
    if ((st & IOB_ST_IN_CHIP) && !(pst & IOB_ST_IN_CHIP)) ev(b, "input chip fault");
    if ((st & IOB_ST_OUT_A_CHIP) && !(pst & IOB_ST_OUT_A_CHIP)) ev(b, "outs 1-8 chip fault");
    if ((st & IOB_ST_COMMS_LOST) && !(pst & IOB_ST_COMMS_LOST)) ev(b, "link lost (board)");
    else if (!(st & IOB_ST_COMMS_LOST) && (pst & IOB_ST_COMMS_LOST)) ev(b, "link fault cleared");
    x->st = st; x->a_open = open; x->a_ovl = ovl; x->a_otemp = otemp; x->wb = wb; x->bf = bf;
    x->have_raw = true;
}

/* IOB_LOST has been acknowledged (or is switched off), so a board's latched comms fault may go */
static bool lost_acknowledged(void) {
    alarm_view_t a;
    memset(&a, 0, sizeof a);
    if (!alarm_get(ALARM_ID_IOB_LOST, &a)) return true;
    return !a.enabled || (a.active && !a.unacked);
}

static void cyclic(unsigned b, const cfg_t *c, uint16_t out, uint8_t rel, uint16_t pwm, uint8_t dir, uint32_t now) {
    bstate_t *x = &B[b];
    uint8_t f[MBR_FRAME_MAX];
    uint16_t r[IOB_R_STATUS_N2];
    x->seq++;
    uint16_t w[5] = { out, rel, x->seq, pwm, dir };
    uint16_t nr = x->map >= 2u ? IOB_R_STATUS_N2 : IOB_R_STATUS_N, nw = x->pwm ? 5u : 3u;
    int rc = transact(b, f, mbr_req_rw(f, c->addr, IOB_R_STATUS, nr, IOB_R_OUT_CMD, w, nw), r, nr);
    if (rc == MBR_OK && r[IOB_R_CMD_ECHO - IOB_R_STATUS] != x->seq) {
        rc = MBR_BAD_FORMAT;                     /* the node did not apply this write */
        PLAT_CRIT_ENTER(); view[b].errors++; view[b].last_error = MBR_BAD_FORMAT; PLAT_CRIT_EXIT();
    }
    if (rc != MBR_OK) { failed(b, now); return; }
    x->last_ok = now;

    uint16_t st = r[0];
    uint8_t open = (uint8_t)r[IOB_R_A_OPEN - IOB_R_STATUS], ovl = (uint8_t)r[IOB_R_A_OVERLOAD - IOB_R_STATUS];
    uint8_t otemp = (uint8_t)r[IOB_R_A_OVERTEMP - IOB_R_STATUS], bf = (uint8_t)r[IOB_R_B_FAULT - IOB_R_STATUS];
    uint8_t wb = (uint8_t)r[IOB_R_WIREBREAK - IOB_R_STATUS];
    log_changes(b, st, open, ovl, otemp, wb, bf, c);

    uint32_t of = (uint32_t)((open & c->ol) | ovl | otemp);
    if (!(st & IOB_ST_FIELD_A_OK) || (st & IOB_ST_OUT_A_CHIP)) of |= 0x00FFu;
    if ((bf & (IOB_BF_FAULT | IOB_BF_WDG)) || !(st & IOB_ST_FIELD_B_OK)) of |= 0xFF00u;
    uint8_t inf = (uint8_t)(wb & c->wb);
    if ((st & IOB_ST_IN_CHIP) || !(st & IOB_ST_FIELD_A_OK)) inf = 0xFF;

    bool clear = false;
    PLAT_CRIT_ENTER();
    iob_view_t *v = &view[b];
    v->inputs = (uint8_t)r[IOB_R_INPUTS - IOB_R_STATUS];
    v->outputs = (uint32_t)r[IOB_R_OUT_APPLIED - IOB_R_STATUS] | (uint32_t)(r[IOB_R_RELAY_APPLIED - IOB_R_STATUS] & 3u) << 16;
    v->out_faults = of;
    v->in_faults = inf;
    v->a_open = open; v->a_overload = ovl; v->a_overtemp = otemp; v->b_fault = bf; v->wirebreak = wb;
    v->node_rx_errors = r[IOB_R_RX_ERRORS - IOB_R_STATUS];
    v->node_resets = r[IOB_R_RESETS - IOB_R_STATUS];
    if (nr == IOB_R_STATUS_N2) {
        v->pwm_applied = r[IOB_R_PWM_APPLIED - IOB_R_STATUS];
        v->rpm = r[IOB_R_TACH_RPM - IOB_R_STATUS];
        v->temp_flags = r[IOB_R_TEMP_FLAGS - IOB_R_STATUS];
        v->temp_count = r[IOB_R_TEMP_COUNT - IOB_R_STATUS];
    }
    uint16_t s = (uint16_t)(v->status & (IOBS_WDG_SEEN | IOBS_TIMEOUT_SEEN));
    s |= IOBS_ONLINE;
    if (st & IOB_ST_OUT_ENABLED) s |= IOBS_GATE_OPEN;
    if (st & IOB_ST_FIELD_A_OK) s |= IOBS_FIELD_A_OK;
    if (st & IOB_ST_FIELD_B_OK) s |= IOBS_FIELD_B_OK;
    if (st & IOB_ST_WDG_RESET) { s |= IOBS_WDG_SEEN; wdg_events[b]++; clear = true; }
    if (st & IOB_ST_TIMEOUT) { s |= IOBS_TIMEOUT_SEEN; clear = true; }
    if (st & IOB_ST_COMMS_LOST) s |= IOBS_COMMS_LATCHED;
    for (unsigned i = 0; i < IOB_TEMPS; ++i)
        if (v->temp_st[i] != IOB_TS_OK && v->temp_st[i] != IOB_TS_OFF && v->temp_st[i] != IOB_TS_WAIT) s |= IOBS_TEMP_FAULT;
    v->status = s;
    PLAT_CRIT_EXIT();

    if (st & IOB_ST_WDG_RESET) ev(b, "watchdog reset");
    if (st & IOB_ST_TIMEOUT) ev(b, "outputs timed out");
    if (st & IOB_ST_CONFIG_NEEDED) { ev(b, "restarted"); x->cfg_sent = false; x->temp_cfg_sent = false; }   /* send the settings again next cycle */
    /* TIMEOUT and WDG_RESET are events, taken in at once. COMMS_LOST stays on the node (its comms
     * LED double-flashes) until someone acknowledges IOB_LOST here, which the node's bit also
     * raises; only then is it cleared on the node (node requirements FW-58). */
    uint16_t fc = clear ? IOB_FC_EVENTS : 0u;
    if ((st & IOB_ST_COMMS_LOST) && lost_acknowledged()) fc |= IOB_FC_COMMS;
    if (fc) {
        uint8_t f2[MBR_FRAME_MAX];
        (void)transact(b, f2, mbr_req_write1(f2, c->addr, IOB_R_FAULT_CLEAR, fc), NULL, 0);
    }
}

static void publish_state(unsigned b) {
    uint8_t s = B[b].state;
    PLAT_CRIT_ENTER();
    iob_view_t *v = &view[b];
    v->state = s;
    if (s != IOB_S_RUN) {
        v->status = (uint16_t)(v->status & (IOBS_WDG_SEEN | IOBS_TIMEOUT_SEEN));
        if (s == IOB_S_LOST) v->status |= IOBS_LOST;
        if (s == IOB_S_WRONG_TYPE) v->status |= IOBS_WRONG_TYPE;
        if (s == IOB_S_BAD_SETTINGS) v->status |= IOBS_BAD_SET;
        if (s == IOB_S_CONNECTING) v->status |= IOBS_CONNECTING;
        v->inputs = 0;
        v->outputs = 0;
        v->out_faults = s == IOB_S_OFF ? 0u : (1u << IOB_OUTS) - 1u;
        v->in_faults = s == IOB_S_OFF ? 0u : 0xFFu;
        v->pwm_applied = v->rpm = 0;
        for (unsigned i = 0; i < IOB_TEMPS; ++i) { v->temp[i] = IOB_TEMP_INVALID; v->temp_st[i] = IOB_TS_WAIT; }
        v->cj = IOB_TEMP_INVALID;
    }
    PLAT_CRIT_EXIT();
}

static void cm_run(void);
static void dc_run(void);

void iob_cycle(uint32_t now) {
    cm_run();                                    /* a commissioning request from the web page, if any */
    dc_run();
    cfg_t c[IOB_BOARDS];
    uint16_t out[IOB_BOARDS], pwm[IOB_BOARDS], timeout;
    uint8_t rel[IOB_BOARDS], dir[IOB_BOARDS], filter;
    bool fresh, valid;
    PLAT_CRIT_ENTER();
    memcpy(c, cmd.cfg, sizeof c);
    memcpy(out, cmd.out, sizeof out);
    memcpy(rel, cmd.rel, sizeof rel);
    memcpy(pwm, cmd.pwm, sizeof pwm);
    memcpy(dir, cmd.dir, sizeof dir);
    timeout = cmd.hold_s;
    filter = cmd.filter_ms;
    uint8_t link = cmd.link;
    valid = cmd.cfg_valid;
    fresh = cmd.out_valid && (uint32_t)(now - cmd.stamp) <= PLAT_IOB_STALE_MS;
    PLAT_CRIT_EXIT();
    if (!valid) return;                          /* the scan has not run yet */
    if (safe || !fresh) { memset(out, 0, sizeof out); memset(rel, 0, sizeof rel); memset(pwm, 0, sizeof pwm); memset(dir, 0, sizeof dir); }
    if (timeout < 1u) timeout = 1u;
    if (timeout > 600u) timeout = 600u;
    if (link != bus_link) {                      /* a new link: every board connects again */
        for (unsigned b = 0; b < IOB_BOARDS; ++b) if (B[b].state != IOB_S_OFF) set_state(b, IOB_S_CONNECTING, now);
        bus_link = link;
    }
    bus = link == IOB_LINK_IOPORT ? io_tcp : link == IOB_LINK_ETH ? tcp : port;
    uint8_t bad = bad_settings(c, link);
    bool temps_read = false;                     /* one board's temperatures per cycle */

    for (unsigned b = 0; b < IOB_BOARDS; ++b) {
        bstate_t *x = &B[b];
        if (!c[b].en) { set_state(b, IOB_S_OFF, now); publish_state(b); continue; }
        if (bad & (1u << b)) { set_state(b, IOB_S_BAD_SETTINGS, now); publish_state(b); continue; }
        if (x->state == IOB_S_OFF || x->state == IOB_S_BAD_SETTINGS) set_state(b, IOB_S_CONNECTING, now);
        if ((x->state == IOB_S_LOST || x->state == IOB_S_WRONG_TYPE) && (int32_t)(now - x->next_try) < 0) { publish_state(b); continue; }
        if (x->state == IOB_S_RUN && x->cfg_sent && x->sent.ip != c[b].ip) set_state(b, IOB_S_CONNECTING, now);  /* another board now */
        if (bus && bus->target) bus->target(bus, c[b].ip);

        if (x->state != IOB_S_RUN) {
            if (!ident(b, c[b].addr)) { failed(b, now); publish_state(b); continue; }
            if (!x->ident_ok) { set_state(b, IOB_S_WRONG_TYPE, now); publish_state(b); continue; }
            x->cfg_sent = false;
            x->temp_cfg_sent = false;
            x->temp_at = now;
        }
        if (cfg_changed(x, &c[b], timeout, filter) && !write_cfg(b, &c[b], timeout, filter)) {
            failed(b, now); publish_state(b); continue;
        }
        if (temp_cfg_changed(x, &c[b]) && !write_cfg2(b, &c[b])) { failed(b, now); publish_state(b); continue; }
        if (x->state != IOB_S_RUN) { x->last_ok = now; set_state(b, IOB_S_RUN, now); }
        cyclic(b, &c[b], out[b], rel[b], pwm[b], dir[b], now);
        if (x->temps && x->state == IOB_S_RUN && !temps_read && (int32_t)(now - x->temp_at) >= 0 && any_sensor(&c[b])) {
            temps_read = true;
            if (!read_temps(b, &c[b], now)) failed(b, now);
        }
        publish_state(b);
    }
}

/* ---------------- commissioning (web page) ---------------- */

const char *iob_cm_result_name(uint8_t r) {
    static const char *const n[] = { "idle", "busy", "ok", "no reply", "not a CEMS I/O board",
                                     "address in use", "refused by the board", "not found at the new address" };
    return r <= IOB_CM_NOT_CONFIRMED ? n[r] : "?";
}

static bool cm_ask(uint8_t op, uint8_t from, uint8_t to) {
    bool ok = false;
    PLAT_CRIT_ENTER();
    if (!cm_req.op && cm_view.result != IOB_CM_BUSY) {
        cm_req.op = op; cm_req.from = from; cm_req.to = to;
        cm_view.result = IOB_CM_BUSY; cm_view.op = op; cm_view.from = from; cm_view.to = to;
        ok = true;
    }
    PLAT_CRIT_EXIT();
    return ok;
}

bool iob_cm_find(uint8_t a) {
    return a >= 1 && a <= IOB_COMMISSION_ADDR && cm_ask(1, a, 0);
}

bool iob_cm_assign(uint8_t from, uint8_t to) {
    return from >= 1 && from <= IOB_COMMISSION_ADDR && to >= 1 && to <= IOB_ADDR_MAX && to != from && cm_ask(2, from, to);
}

void iob_cm_view(iob_cm_view_t *v) {
    PLAT_CRIT_ENTER();
    *v = cm_view;
    PLAT_CRIT_EXIT();
}

/* one request outside the boards' own (their counters are not touched) */
static int cm_xfer(const uint8_t *tx, size_t n, uint16_t *regs, uint16_t n_regs, uint8_t *exc) {
    uint8_t rx[MBR_FRAME_MAX];
    uint16_t got = 0;
    if (n == 0) return MBR_BAD_FORMAT;
    if (port && port->xfer) got = port->xfer(port, tx, (uint16_t)n, rx, sizeof rx, PLAT_IOB_REPLY_MS);
    return mbr_check_reply(tx, n, rx, got, regs, n_regs, exc);
}

/* identification and serial number of whatever answers at a; IOB_CM_OK, NO_REPLY or WRONG_TYPE */
static uint8_t cm_read(uint8_t a, iob_cm_view_t *v) {
    uint8_t f[MBR_FRAME_MAX];
    uint16_t r[IOB_R_IDENT_N], s[IOB_SERIAL_REGS];
    int rc = MBR_NO_REPLY;
    for (int t = 0; t < 2 && rc != MBR_OK && rc != MBR_EXCEPTION; ++t)   /* one retry for a damaged frame */
        rc = cm_xfer(f, mbr_req_read(f, a, IOB_R_MAGIC, IOB_R_IDENT_N), r, IOB_R_IDENT_N, NULL);
    if (rc == MBR_EXCEPTION) return IOB_CM_WRONG_TYPE;
    if (rc != MBR_OK) return IOB_CM_NO_REPLY;
    if (r[0] != IOB_MAGIC || r[1] < 1u || r[1] > IOB_MAP_VERSION) return IOB_CM_WRONG_TYPE;
    v->found = true;
    v->hw_rev = r[IOB_R_HW_REV - IOB_R_MAGIC];
    v->fw_version = r[IOB_R_FW_VERSION - IOB_R_MAGIC];
    v->fw_build = r[IOB_R_FW_BUILD - IOB_R_MAGIC];
    memcpy(v->uid, &r[IOB_R_UID - IOB_R_MAGIC], sizeof v->uid);
    if (cm_xfer(f, mbr_req_read(f, a, IOB_R_SERIAL, IOB_SERIAL_REGS), s, IOB_SERIAL_REGS, NULL) == MBR_OK) {
        for (unsigned k = 0; k < IOB_SERIAL_REGS; ++k) {
            v->serial[2 * k] = (char)(s[k] >> 8);
            v->serial[2 * k + 1] = (char)(s[k] & 0xFFu);
        }
        for (unsigned k = 0; k < 2 * IOB_SERIAL_REGS; ++k)
            if (v->serial[k] < 0x20 || v->serial[k] > 0x7E) { v->serial[k] = 0; break; }
    }
    return IOB_CM_OK;
}

static void cm_log(uint8_t to, const char *text) {
    uint32_t board = 0;
    PLAT_CRIT_ENTER();
    for (unsigned b = 0; b < IOB_BOARDS; ++b) if (cmd.cfg[b].addr == to) { board = b + 1u; break; }
    PLAT_CRIT_EXIT();
    plat_log_event(LOG_EV_IOB, PLAT_SRC_WEB, 0, board, NAN, NAN, text);
}

static void cm_run(void) {
    uint8_t op, from, to;
    PLAT_CRIT_ENTER();
    op = cm_req.op; from = cm_req.from; to = cm_req.to;
    PLAT_CRIT_EXIT();
    if (!op) return;
    iob_cm_view_t v;
    memset(&v, 0, sizeof v);
    v.op = op; v.from = from; v.to = to;
    if (op == 1) v.result = cm_read(from, &v);
    else {
        iob_cm_view_t other;
        memset(&other, 0, sizeof other);
        uint8_t at_to = cm_read(to, &other);
        if (at_to != IOB_CM_NO_REPLY) v.result = IOB_CM_IN_USE;   /* never two devices at one address */
        else {
            uint8_t f[MBR_FRAME_MAX];
            uint16_t w[3] = { IOB_ADDR_SET_KEY, to, (uint16_t)(to ^ 0xFFFFu) };
            uint8_t exc = 0;
            int rc = cm_xfer(f, mbr_req_write(f, from, IOB_R_ADDR_SET, w, 3), NULL, 0, &exc);
            if (rc == MBR_EXCEPTION) { v.result = IOB_CM_REFUSED; v.exception = exc; }
            else if (rc != MBR_OK) v.result = IOB_CM_NO_REPLY;
            else v.result = cm_read(to, &v) == IOB_CM_OK ? IOB_CM_OK : IOB_CM_NOT_CONFIRMED;
            char t[23];
            snprintf(t, sizeof t, "addr %u->%u %s", (unsigned)from, (unsigned)to, v.result == IOB_CM_OK ? "set" : "failed");
            cm_log(to, t);
        }
    }
    PLAT_CRIT_ENTER();
    v.seq = cm_view.seq + 1u;
    cm_view = v;
    cm_req.op = 0;
    PLAT_CRIT_EXIT();
}

/* ---------------- Ethernet commissioning (iob_dc_*) ---------------- */
static uint16_t dc_seq;

bool iob_dc_find(void) {
    bool ok = false;
    PLAT_CRIT_ENTER();
    if (!dc_req.op && dc_view.result != IOB_CM_BUSY) {
        dc_req.op = 1;
        dc_view.result = IOB_CM_BUSY; dc_view.op = 1;
        ok = true;
    }
    PLAT_CRIT_EXIT();
    return ok;
}

bool iob_dc_set(const uint16_t uid[6], uint32_t ip, uint32_t mask, uint32_t gw, uint32_t master_ip, uint8_t address) {
    if (!iob_net_settings_ok(ip, mask, gw, master_ip, address)) return false;
    bool ok = false;
    PLAT_CRIT_ENTER();
    if (!dc_req.op && dc_view.result != IOB_CM_BUSY) {
        memset(&dc_req.set, 0, sizeof dc_req.set);
        dc_req.op = 2;
        dc_req.set.op = IOB_DISC_SET;
        memcpy(dc_req.set.uid, uid, sizeof dc_req.set.uid);
        dc_req.set.ip = ip; dc_req.set.mask = mask; dc_req.set.gw = gw;
        dc_req.set.master_ip = master_ip; dc_req.set.address = address;
        dc_view.result = IOB_CM_BUSY; dc_view.op = 2;
        ok = true;
    }
    PLAT_CRIT_EXIT();
    return ok;
}

void iob_dc_view(iob_dc_view_t *v) {
    PLAT_CRIT_ENTER();
    *v = dc_view;
    PLAT_CRIT_EXIT();
}

/* send one message, then collect HERE answers to it for PLAT_IOB_DISC_WAIT_MS (or until want is seen) */
static unsigned dc_exchange(const iob_disc_t *m, iob_disc_t *list, unsigned max, const uint16_t *want) {
    uint8_t b[IOB_DISC_LEN];
    (void)iob_disc_encode(m, b);
    uint8_t link;
    PLAT_CRIT_ENTER(); link = cmd.cfg_valid ? cmd.link : iob_link; PLAT_CRIT_EXIT();
    iob_dgram_t *d = link == IOB_LINK_IOPORT ? io_disc : disc;   /* RS-485 has none: the main port's, as before */
    if (!d || !d->send || !d->send(d, b, IOB_DISC_LEN)) return 0;
    unsigned n = 0;
    uint32_t t0 = bsp_ms();
    for (;;) {
        uint32_t el = bsp_ms() - t0;
        if (el >= PLAT_IOB_DISC_WAIT_MS) break;
        uint16_t got = d->recv(d, b, sizeof b, PLAT_IOB_DISC_WAIT_MS - el);
        iob_disc_t h;
        if (!got) break;
        if (!iob_disc_decode(b, got, &h) || h.op != IOB_DISC_HERE || h.seq != m->seq) continue;
        bool dup = false;
        for (unsigned k = 0; k < n; ++k) dup |= memcmp(list[k].uid, h.uid, sizeof h.uid) == 0;
        if (dup) continue;
        if (n < max) list[n++] = h;
        if (want && memcmp(h.uid, want, sizeof h.uid) == 0) break;
    }
    return n;
}

static void dc_run(void) {
    uint8_t op;
    iob_disc_t set;
    PLAT_CRIT_ENTER();
    op = dc_req.op; set = dc_req.set;
    PLAT_CRIT_EXIT();
    if (!op) return;
    iob_disc_t list[IOB_DC_MAX];
    uint8_t result, n_list = 0;
    if (op == 1) {
        iob_disc_t f; memset(&f, 0, sizeof f);
        f.op = IOB_DISC_FIND; f.seq = ++dc_seq;
        n_list = (uint8_t)dc_exchange(&f, list, IOB_DC_MAX, NULL);
        result = n_list ? IOB_CM_OK : IOB_CM_NO_REPLY;
    } else {
        /* the new IP must not be the supervisor's or another board's in the last list */
        bool used = set.ip == set.master_ip;
        PLAT_CRIT_ENTER();
        for (unsigned k = 0; k < dc_view.n; ++k)
            if (dc_view.boards[k].ip == set.ip && memcmp(dc_view.boards[k].uid, set.uid, sizeof set.uid) != 0) used = true;
        PLAT_CRIT_EXIT();
        if (used) result = IOB_CM_IN_USE;
        else {
            set.seq = ++dc_seq;
            iob_disc_t got[IOB_DC_MAX];
            unsigned m = dc_exchange(&set, got, IOB_DC_MAX, set.uid);
            result = IOB_CM_NO_REPLY;
            for (unsigned k = 0; k < m; ++k) {
                if (memcmp(got[k].uid, set.uid, sizeof set.uid) != 0) continue;
                bool same = got[k].ip == set.ip && got[k].mask == set.mask && got[k].gw == set.gw &&
                            got[k].master_ip == set.master_ip && got[k].address == set.address;
                result = same ? IOB_CM_OK : IOB_CM_REFUSED;
                list[0] = got[k]; n_list = 1;
            }
            char t[23];
            snprintf(t, sizeof t, "IP .%u.%u %s", (unsigned)((set.ip >> 8) & 255u), (unsigned)(set.ip & 255u),
                     result == IOB_CM_OK ? "set" : "failed");
            uint32_t board = 0;
            PLAT_CRIT_ENTER();
            for (unsigned b = 0; b < IOB_BOARDS; ++b) if (cmd.cfg[b].ip == set.ip) { board = b + 1u; break; }
            PLAT_CRIT_EXIT();
            plat_log_event(LOG_EV_IOB, PLAT_SRC_WEB, 0, board, NAN, NAN, t);
        }
    }
    PLAT_CRIT_ENTER();
    if (op == 1 || n_list) {                      /* a set keeps the list, with that board's new settings */
        if (op == 1) { memcpy(dc_view.boards, list, sizeof(iob_disc_t) * n_list); dc_view.n = n_list; }
        else for (unsigned k = 0; k < dc_view.n; ++k)
            if (memcmp(dc_view.boards[k].uid, list[0].uid, sizeof list[0].uid) == 0) dc_view.boards[k] = list[0];
    }
    dc_view.result = result;
    dc_view.op = op;
    dc_view.seq++;
    dc_req.op = 0;
    PLAT_CRIT_EXIT();
}

/* ---------------- scan side ---------------- */
static uint32_t wdg_seen[IOB_BOARDS];

void iob_scan_in(plat_io_in_t *in, uint32_t now_ms) {
    (void)now_ms;
    iob_view_t v[IOB_BOARDS];
    uint32_t wd[IOB_BOARDS];
    PLAT_CRIT_ENTER();
    for (unsigned b = 0; b < IOB_BOARDS; ++b) {
        const iob_board_tags_t *t = &iob_tags[b];
        cfg_t *cf = &cmd.cfg[b];
        *cf = (cfg_t){ .en = t->enable, .addr = t->address, .ip = t->ip, .first_do = t->first_do,
                       .first_di = t->first_di, .ol = t->ol_mask, .wb = t->wb_mask };
        for (unsigned k = 0; k < IOB_TEMP_CONNS; ++k) {
            cf->temp[IOB_R_TEMP_TYPE - IOB_R_TEMP_TYPE + k] = tt_known(t->temp_type[k]) ? t->temp_type[k] : IOB_TT_NONE;
            cf->temp[IOB_R_NTC_R25 - IOB_R_TEMP_TYPE + k] = t->ntc_r25[k] >= 10u ? t->ntc_r25[k] : 1000u;
            cf->temp[IOB_R_NTC_BETA - IOB_R_TEMP_TYPE + k] = t->ntc_beta[k] >= 1000u && t->ntc_beta[k] <= 10000u ? t->ntc_beta[k] : 3435u;
        }
        cf->temp[IOB_R_PWM_FREQ - IOB_R_TEMP_TYPE] = t->pwm_hz >= 10u && t->pwm_hz <= 25000u ? t->pwm_hz : 1000u;
        cf->temp[IOB_R_TACH_PPR - IOB_R_TEMP_TYPE] = t->tach_ppr >= 1u && t->tach_ppr <= 12u ? t->tach_ppr : 2u;
    }
    cmd.hold_s = iob_hold_s;
    cmd.filter_ms = iob_filter_ms;
    cmd.link = iob_link;
    cmd.cfg_valid = true;
    memcpy(v, view, sizeof v);
    memcpy(wd, wdg_events, sizeof wd);
    PLAT_CRIT_EXIT();

    bool lost = false, conf = false, of = false, inf = false, sup = false, wdg = false, tf = false;
    for (unsigned b = 0; b < IOB_BOARDS; ++b) {
        iob_board_tags_t *t = &iob_tags[b];
        bool on = t->enable && v[b].state != IOB_S_OFF && v[b].state != IOB_S_BAD_SETTINGS;
        if (on) {
            unsigned d0 = t->first_di - 1u, o0 = t->first_do - 1u;
            uint64_t dm = (uint64_t)0xFFu << d0, om = (uint64_t)((1u << IOB_OUTS) - 1u) << o0;
            in->di = (in->di & ~dm) | ((uint64_t)v[b].inputs << d0);
            in->di_fault = (in->di_fault & ~dm) | ((uint64_t)v[b].in_faults << d0);
            in->do_fault = (in->do_fault & ~om) | ((uint64_t)v[b].out_faults << o0);
        }
        /* temperatures (map 2): calibrated here; any channel without a reading is a fault for the application */
        for (unsigned i = 0; i < IOB_TEMPS; ++i) {
            uint8_t typ = t->temp_type[i / 2u], st = v[b].temp_st[i];
            bool used = t->enable && tt_known(typ) && typ != IOB_TT_NONE && (typ <= IOB_TT_TC_S || i % 2u == 0);
            bool ok = on && v[b].state == IOB_S_RUN && v[b].n_temps && st == IOB_TS_OK;
            int32_t cd = ok ? iob_temp_cal(v[b].temp[i], t->temp_offset[i], t->temp_gain[i]) : 0;
            t->temps[i] = (int16_t)(ok ? (cd >= 0 ? (cd + 5) / 10 : (cd - 5) / 10) : IOB_TEMP_INVALID);
            t->temp_state[i] = !used ? (uint8_t)IOB_TS_OFF : !on || v[b].state != IOB_S_RUN ? (uint8_t)IOB_TS_WAIT
                               : !v[b].n_temps ? (uint8_t)IOB_TS_ADC : st;
            unsigned ch = t->first_temp ? t->first_temp - 1u + i : PLAT_RTD_MAX;
            if (used && ch < PLAT_RTD_MAX) {
                uint8_t ps = PLAT_AIN_FAULT;
                if (ok) ps = PLAT_AIN_OK;
                else if (st == IOB_TS_UNDER) ps = PLAT_AIN_UNDER;
                else if (st == IOB_TS_OVER) ps = PLAT_AIN_OVER;
                in->rtd_cdeg[ch] = cd;
                in->rtd_status[ch] = ps;
            }
            if (used && v[b].state == IOB_S_RUN && t->temp_state[i] > IOB_TS_WAIT) tf = true;
        }
        t->cj_temp = v[b].cj;
        t->pump_rpm = v[b].rpm;
        t->pump_duty = v[b].pwm_applied;
        t->status = v[b].status;
        t->out_faults = v[b].out_faults;
        t->in_faults = v[b].in_faults;
        t->errors = v[b].errors;
        t->inputs = v[b].inputs;
        t->outputs = v[b].outputs;
        if (!t->enable) { wdg_seen[b] = wd[b]; continue; }
        if (v[b].state == IOB_S_LOST) lost = true;
        if (v[b].state == IOB_S_RUN && (v[b].status & IOBS_COMMS_LATCHED)) lost = true;   /* the board saw a loss */
        if (v[b].state == IOB_S_WRONG_TYPE || v[b].state == IOB_S_BAD_SETTINGS) conf = true;
        if (v[b].state == IOB_S_RUN) {
            if (v[b].out_faults) of = true;
            if (v[b].in_faults) inf = true;
            if ((v[b].status & (IOBS_FIELD_A_OK | IOBS_FIELD_B_OK)) != (IOBS_FIELD_A_OK | IOBS_FIELD_B_OK)) sup = true;
        }
        if (wd[b] != wdg_seen[b]) { wdg = true; wdg_seen[b] = wd[b]; }
    }
    alarm_set(ALARM_ID_IOB_LOST, lost);
    alarm_set(ALARM_ID_IOB_CONFIG, conf);
    alarm_set(ALARM_ID_IOB_OUT_FAULT, of);
    alarm_set(ALARM_ID_IOB_IN_FAULT, inf);
    alarm_set(ALARM_ID_IOB_SUPPLY, sup);
    alarm_set(ALARM_ID_IOB_NODE_RESET, wdg);
    alarm_set(ALARM_ID_IOB_TEMP_FAULT, tf);
}

void iob_scan_out(const plat_io_out_t *o, uint32_t now_ms) {
    uint16_t out[IOB_BOARDS] = { 0 }, pwm[IOB_BOARDS] = { 0 };
    uint8_t rel[IOB_BOARDS] = { 0 }, dir[IOB_BOARDS] = { 0 };
    for (unsigned b = 0; b < IOB_BOARDS; ++b) {
        const iob_board_tags_t *t = &iob_tags[b];
        if (t->enable && t->first_ao >= 1 && t->first_ao <= PLAT_AO_MAX) {
            int32_t a = o->ao_raw[t->first_ao - 1u];
            dir[b] = a < 0;
            a = a < 0 ? -a : a;
            pwm[b] = (uint16_t)(a > 1000 ? 1000 : a);
        }
        if (!t->enable || t->first_do < 1 || t->first_do - 1u + IOB_OUTS > PLAT_DO_MAX) continue;
        uint32_t bits = (uint32_t)(o->do_bits >> (t->first_do - 1u)) & ((1u << IOB_OUTS) - 1u);
        out[b] = (uint16_t)bits;
        rel[b] = (uint8_t)(bits >> 16);
    }
    PLAT_CRIT_ENTER();
    memcpy(cmd.out, out, sizeof out);
    memcpy(cmd.rel, rel, sizeof rel);
    memcpy(cmd.pwm, pwm, sizeof pwm);
    memcpy(cmd.dir, dir, sizeof dir);
    cmd.stamp = now_ms;
    cmd.out_valid = true;
    PLAT_CRIT_EXIT();
}

void plat_hw_io_read(plat_io_in_t *in) {
    bsp_io_read(in);
    iob_scan_in(in, bsp_ms());
}

void plat_hw_io_write(const plat_io_out_t *out) {
    bsp_io_write(out);
    iob_scan_out(out, bsp_ms());
}

void plat_io_safe_state(void) {
    iob_safe_state();
    bsp_io_safe_state();
}
