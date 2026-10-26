/* test_modbus.c - Modbus engine on a map shaped like the old UniLogic slave map
 * (stage4/4G/modbus_map.csv): INT16 status registers, 32-bit timer presets,
 * a REAL, 10-register name buffers, bit arrays on coils.
 * Requirements proved here: CIP-01.6, 10.5, 14.2, 14.4, 15.1, 15.2, 13.3. */
#include <string.h>
#include "tst.h"
#include "modbus.h"
#include "plat_clock.h"
#include "plat_config.h"
#include "mbtcp.h"
#include "plat_state.h"

extern uint8_t plat_meas_q[PLAT_MEAS_MAX];    /* plat_state.c */

static int16_t  m_mode, m_count;          /* status, read-only */
static uint32_t m_preset;                 /* like ModbusTimer.Preset (ms) */
static float    m_flow;                   /* REAL */
static float    m_temp;                   /* shown x10 as INT16 */
static int16_t  m_route[4];               /* route registers 6-9 (1-based valves) */
static char     m_name[20];               /* 10-register name, text */
static uint8_t  m_buf[20];                /* raw buffer */
static bool     m_alarms[22];             /* coil bit array */
static bool     m_ack[9];                 /* writable coil bits */
static bool     m_start;                  /* start coil */
static bool     m_di;                     /* discrete input */
static uint16_t m_inreg;                  /* input register */

enum { P_MODE = 3000, P_COUNT, P_PRESET, P_FLOW, P_TEMP, P_ROUTE, P_NAME, P_BUF, P_ALARMS, P_ACK, P_START, P_DI, P_IN };

static const param_def_t defs[] = {
    PARAM_INT (P_MODE,  "mode",  m_mode,  CT_I16, -1, 10, 0, PLAT_LVL_VIEW, PF_RO, NULL),
    PARAM_INT (P_COUNT, "count", m_count, CT_I16, 0, 20, 0, PLAT_LVL_VIEW, PF_RO, NULL),
    PARAM_UINT(P_PRESET,"preset",m_preset,CT_U32, 0, 86400000u, 0, PLAT_LVL_OPERATOR, 0, "ms"),
    PARAM_REAL(P_FLOW,  "flow",  m_flow, -100.0f, 100.0f, 0.0f, "l/min", PLAT_LVL_OPERATOR, 0),
    PARAM_REAL(P_TEMP,  "temp",  m_temp, -50.0f, 200.0f, 0.0f, "C", PLAT_LVL_OPERATOR, 0),
    PARAM_ARRAY_INT(P_ROUTE, "route", m_route, CT_I16, 0, 19, PLAT_LVL_OPERATOR, 0),
    PARAM_TEXT (P_NAME,  "name",  m_name, NULL, PLAT_LVL_OPERATOR, 0),
    PARAM_BYTES(P_BUF,   "buf",   m_buf, PLAT_LVL_OPERATOR, 0),
    PARAM_ARRAY_BOOL(P_ALARMS, "alarms", m_alarms, PLAT_LVL_VIEW, PF_RO),
    PARAM_ARRAY_BOOL(P_ACK,    "ack",    m_ack, PLAT_LVL_OPERATOR, PF_ANY_MODE),
    PARAM_BIT (P_START, "start", m_start, PLAT_LVL_OPERATOR, 0),
    PARAM_BIT (P_DI,    "di",    m_di, PLAT_LVL_VIEW, PF_RO),
    PARAM_UINT(P_IN,    "inreg", m_inreg, CT_U16, 0, 65535, 0, PLAT_LVL_VIEW, PF_RO, NULL),
};

static const mb_map_t map[] = {
    MB_ENTRY  (MB_HOLDING, 0,  P_MODE,   MBF_I16),
    MB_ENTRY  (MB_HOLDING, 1,  P_COUNT,  MBF_I16),
    MB_ENTRY  (MB_HOLDING, 6,  P_ROUTE,  MBF_ARRAY),          /* 6..9 */
    MB_ENTRY  (MB_HOLDING, 10, P_PRESET, MBF_U32),            /* 10..11, high word first */
    MB_ENTRY_X(MB_HOLDING, 12, P_FLOW,   MBF_F32, MBF_WORD_LO_FIRST, 0),
    MB_ENTRY_X(MB_HOLDING, 14, P_TEMP,   MBF_I16, 0, 1),      /* x10 */
    MB_ENTRY  (MB_HOLDING, 20, P_NAME,   MBF_BYTES),          /* 20..29 */
    MB_ENTRY_X(MB_HOLDING, 30, P_BUF,    MBF_BYTES, MBF_BYTE_LO_FIRST, 0),   /* 30..39 */
    MB_ENTRY  (MB_COILS,   1,  P_START,  MBF_BIT),
    MB_ENTRY  (MB_COILS,   13, P_ALARMS, MBF_ARRAY),          /* 13..34 */
    MB_ENTRY  (MB_COILS,   90, P_ACK,    MBF_ARRAY),          /* 90..98 */
    MB_ENTRY  (MB_DISCRETE, 0, P_DI,     MBF_BIT),
    MB_ENTRY  (MB_INPUT,    0, P_IN,     MBF_U16),
};

static plat_cmdq_t q;
static uint8_t rsp[300];
static uint16_t rl;

static void scan(void) {
    plat_cmd_t c; while (plat_cmdq_pop(&q, &c)) (void)param_apply(&c);
    param_blob_drain();
    param_publish();
}
static void remote(bool on) {
    plat_cmd_t c = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR, .kind = PLAT_VAL_U32, .v.u = on };
    (void)param_apply(&c);
}
static void setup(void) {
    CHECK_EQ(param_init(defs, sizeof defs / sizeof defs[0]), PERR_OK);
    param_set_defaults(PARAM_ALL);
    plat_cmdq_init(&q);
    param_bind(&q, NULL);
    mb_ident_t id = { "Acme", "CEMS Supervisor", "1.2.3", PLAT_LVL_OPERATOR, 7 };
    CHECK_EQ(mb_init(map, sizeof map / sizeof map[0], &id), 0);
    remote(true);
    scan();
}

static uint16_t go(const uint8_t *req, uint16_t n) { rl = mb_pdu(req, n, rsp, sizeof rsp); return rl; }
static uint16_t reg(int k) { return (uint16_t)((rsp[2 + 2 * k] << 8) | rsp[3 + 2 * k]); }
static void rd(uint8_t fc, uint16_t a, uint16_t n) {
    uint8_t r[5] = { fc, (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(n >> 8), (uint8_t)n }; go(r, 5);
}
static void wr16(uint16_t a, const uint16_t *v, uint16_t n) {
    uint8_t r[256] = { 16, (uint8_t)(a >> 8), (uint8_t)a, 0, (uint8_t)n, (uint8_t)(2 * n) };
    for (uint16_t k = 0; k < n; ++k) { r[6 + 2 * k] = (uint8_t)(v[k] >> 8); r[7 + 2 * k] = (uint8_t)v[k]; }
    go(r, (uint16_t)(6 + 2 * n));
}
#define EXC(code) (rl == 2 && (rsp[0] & 0x80) && rsp[1] == (code))

static void t_map_validation(void) {
    setup();
    mb_map_t bad1[2] = { MB_ENTRY(MB_HOLDING, 0, P_PRESET, MBF_U32), MB_ENTRY(MB_HOLDING, 1, P_MODE, MBF_I16) };  /* overlap */
    CHECK_EQ(mb_init(bad1, 2, NULL), 2);
    mb_map_t bad2[1] = { MB_ENTRY(MB_COILS, 0, P_PRESET, MBF_U32) };          /* register format on coils */
    CHECK_EQ(mb_init(bad2, 1, NULL), 1);
    mb_map_t bad3[1] = { MB_ENTRY(MB_HOLDING, 0, 9999, MBF_U16) };            /* unknown ID */
    CHECK_EQ(mb_init(bad3, 1, NULL), 1);
    mb_map_t bad4[1] = { MB_ENTRY(MB_HOLDING, 65535, P_PRESET, MBF_U32) };    /* past the end */
    CHECK_EQ(mb_init(bad4, 1, NULL), 1);
    mb_map_t bad5[1] = { MB_ENTRY(MB_COILS, 0, P_ROUTE, MBF_ARRAY) };         /* int array on coils */
    CHECK_EQ(mb_init(bad5, 1, NULL), 1);
    mb_map_t e = MB_ENTRY(MB_HOLDING, 20, P_NAME, MBF_BYTES);
    CHECK_EQ(mb_entry_span(&e), 10);
    setup();
}

static void t_read_registers(void) {
    setup();
    m_mode = -1; m_count = 7; m_preset = 0x00012345u; m_flow = 1.5f; m_temp = 23.46f;
    m_route[0] = 3; m_route[3] = 19;
    strcpy(m_name, "Inlet 1");
    m_buf[0] = 'A'; m_buf[1] = 'B';
    scan();
    rd(3, 0, 2); CHECK_EQ(rl, 6); CHECK_EQ(reg(0), 0xFFFF); CHECK_EQ(reg(1), 7);
    rd(3, 6, 9);                                          /* route, preset, flow, temp in one read */
    CHECK_EQ(rl, 20);
    CHECK_EQ(reg(0), 3); CHECK_EQ(reg(3), 19);
    CHECK_EQ(reg(4), 0x0001); CHECK_EQ(reg(5), 0x2345);  /* high word first */
    CHECK_EQ(reg(6), 0x0000); CHECK_EQ(reg(7), 0x3FC0);  /* 1.5f = 0x3FC00000, low word first */
    CHECK_EQ(reg(8), 235);                               /* 23.46 x 10, rounded */
    rd(3, 20, 4);
    CHECK_EQ(reg(0), ('I' << 8) | 'n'); CHECK_EQ(reg(3), ('1' << 8) | 0);
    rd(3, 30, 1); CHECK_EQ(reg(0), ('B' << 8) | 'A');    /* low byte first */
    rd(3, 11, 1); CHECK_EQ(reg(0), 0x2345);              /* reading half of a 32-bit value is allowed */
    rd(4, 0, 1); CHECK_EQ(rl, 4);
    m_temp = -7.25f; scan(); rd(3, 14, 1); CHECK_EQ(reg(0), (uint16_t)(int16_t)-73);
    m_temp = 199.0f; m_preset = 0; scan(); rd(3, 14, 1); CHECK_EQ(reg(0), 1990);
}

static void t_read_errors(void) {
    setup();
    rd(3, 2, 1); CHECK(EXC(2));                           /* unmapped */
    rd(3, 0, 3); CHECK(EXC(2));                           /* runs into a gap */
    rd(3, 0, 0); CHECK(EXC(3));
    rd(3, 0, 126); CHECK(EXC(3));
    rd(4, 1, 1); CHECK(EXC(2));
    uint8_t r[4] = { 3, 0, 0, 1 }; go(r, 4); CHECK(EXC(3));   /* short PDU */
    uint8_t u[1] = { 0x11 }; go(u, 1); CHECK(EXC(1));         /* unsupported function */
    CHECK(mb_stats().exceptions >= 7);
}

static void t_bits(void) {
    setup();
    m_alarms[0] = true; m_alarms[9] = true; m_alarms[21] = true; m_start = true; m_di = true;
    scan();
    rd(1, 13, 22); CHECK_EQ(rl, 5);
    CHECK_EQ(rsp[2], 0x01); CHECK_EQ(rsp[3], 0x02); CHECK_EQ(rsp[4], 0x20);
    rd(1, 1, 1); CHECK_EQ(rsp[2], 1);
    rd(2, 0, 1); CHECK_EQ(rsp[2], 1);
    rd(1, 2, 1); CHECK(EXC(2));
    rd(1, 13, 2001); CHECK(EXC(3));
}

static void t_write_single(void) {
    setup();
    uint8_t c5[5] = { 5, 0, 1, 0xFF, 0x00 };
    go(c5, 5); CHECK_EQ(rl, 5); CHECK(memcmp(rsp, c5, 5) == 0);   /* echo */
    CHECK(!m_start); scan(); CHECK(m_start);                       /* applied by the scan */
    uint8_t c5bad[5] = { 5, 0, 1, 0x12, 0x34 }; go(c5bad, 5); CHECK(EXC(3));
    uint8_t c5ro[5] = { 5, 0, 13, 0xFF, 0x00 }; go(c5ro, 5); CHECK(EXC(2));   /* alarm bits are read-only */
    uint8_t c6[5] = { 6, 0, 14, 0xFF, 0xF6 };                       /* temp = -10 / 10 = -1.0 */
    go(c6, 5); CHECK_EQ(rl, 5); scan(); CHECK(m_temp == -1.0f);
    uint8_t c6ro[5] = { 6, 0, 0, 0, 1 }; go(c6ro, 5); CHECK(EXC(2));         /* status register */
    uint8_t c6half[5] = { 6, 0, 10, 0, 1 }; go(c6half, 5); CHECK(EXC(2));    /* half of a 32-bit value */
    uint8_t c6rng[5] = { 6, 0, 14, 0x27, 0x10 }; go(c6rng, 5); CHECK(EXC(3)); /* 1000.0 > max 200 */
}

static void t_write_multiple(void) {
    setup();
    uint16_t v[4] = { 0x0001, 0x5180, 0, 0 };                        /* preset = 86400 */
    wr16(10, v, 2); CHECK_EQ(rl, 5); scan(); CHECK_EQ(m_preset, 86400u);
    float f = -12.5f; uint32_t u; memcpy(&u, &f, 4);
    uint16_t fv[2] = { (uint16_t)u, (uint16_t)(u >> 16) };          /* low word first */
    wr16(12, fv, 2); scan(); CHECK(m_flow == -12.5f);
    uint16_t route[4] = { 1, 2, 3, 4 };
    wr16(6, route, 4); scan(); CHECK_EQ(m_route[0], 1); CHECK_EQ(m_route[3], 4);
    /* atomic: the last value is out of range, so nothing changes */
    uint16_t mixed[6] = { 5, 6, 7, 20, 0x0000, 0x0001 };            /* route[3] = 20 > 19 */
    wr16(6, mixed, 6); CHECK(EXC(3)); scan();
    CHECK_EQ(m_route[0], 1); CHECK_EQ(m_preset, 86400u);
    /* a write that ends half way through a 32-bit value */
    uint16_t three[3] = { 1, 2, 3 }; wr16(8, three, 3); CHECK(EXC(2));
    /* bad byte count */
    uint8_t r[9] = { 16, 0, 6, 0, 1, 3, 0, 1, 0 }; go(r, 9); CHECK(EXC(3));
    /* coils, FC15: ack bits 90..98 */
    uint8_t c15[8] = { 15, 0, 90, 0, 9, 2, 0x05, 0x01 };            /* bits 0, 2, 8 */
    go(c15, 8); CHECK_EQ(rl, 5); scan();
    CHECK(m_ack[0]); CHECK(!m_ack[1]); CHECK(m_ack[2]); CHECK(m_ack[8]);
}

static void t_text_and_bytes(void) {
    setup();
    uint16_t nm[10] = { ('S' << 8) | 't', ('a' << 8) | 'c', ('k' << 8) | ' ', ('A' << 8) | 0, 0, 0, 0, 0, 0, 0 };
    wr16(20, nm, 10); CHECK_EQ(rl, 5); scan();
    CHECK(strcmp(m_name, "Stack A") == 0);
    wr16(20, nm, 4); CHECK(EXC(3));                                  /* part of a text entry */
    uint16_t full[10]; for (int k = 0; k < 10; ++k) full[k] = ('x' << 8) | 'y';
    wr16(20, full, 10); CHECK(EXC(3));                               /* 20 chars + NUL won't fit */
    uint16_t bb[2] = { 0x4241, 0x4443 };                             /* low byte first: "ABCD" */
    wr16(32, bb, 2); scan();
    CHECK_EQ(m_buf[4], 'A'); CHECK_EQ(m_buf[5], 'B'); CHECK_EQ(m_buf[7], 'D'); CHECK_EQ(m_buf[0], 0);
}

static void t_permissions(void) {
    setup();
    remote(false);                                                   /* LOCAL mode */
    uint8_t c5[5] = { 5, 0, 1, 0xFF, 0x00 }; go(c5, 5); CHECK(EXC(4));
    uint8_t ack[5] = { 5, 0, 90, 0xFF, 0x00 }; go(ack, 5); CHECK_EQ(rl, 5);   /* ANY_MODE */
    remote(true);
    plat_cmd_t c = { .id = PARAM_ID_MODBUS_WRITE_EN, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_ADMIN, .kind = PLAT_VAL_BOOL, .v.b = false };
    CHECK_EQ(param_apply(&c), PERR_OK);
    go(c5, 5); CHECK(EXC(1));                                        /* Modbus read-only */
    rd(3, 0, 1); CHECK_EQ(rl, 4);                                    /* reads still work */
    c.v.b = true; (void)param_apply(&c);
    go(c5, 5); CHECK_EQ(rl, 5);
}

static void t_busy(void) {
    setup();
    for (unsigned k = 0; k < PLAT_CMDQ_DEPTH; ++k) { uint8_t c5[5] = { 5, 0, 1, 0xFF, 0x00 }; go(c5, 5); }
    uint8_t c5[5] = { 5, 0, 1, 0xFF, 0x00 }; go(c5, 5); CHECK(EXC(6));
    uint16_t v[2] = { 0, 1 }; wr16(10, v, 2); CHECK(EXC(6));
    scan();
    go(c5, 5); CHECK_EQ(rl, 5);
}

static void t_device_id(void) {
    setup();
    uint8_t r[4] = { 0x2B, 0x0E, 1, 0 }; go(r, 4);
    CHECK_EQ(rsp[0], 0x2B); CHECK_EQ(rsp[6], 3);
    CHECK_EQ(rsp[7], 0); CHECK_EQ(rsp[8], 4); CHECK(memcmp(rsp + 9, "Acme", 4) == 0);
    uint8_t r4[4] = { 0x2B, 0x0E, 4, 2 }; go(r4, 4);
    CHECK_EQ(rsp[6], 1); CHECK_EQ(rsp[7], 2); CHECK(memcmp(rsp + 9, "1.2.3", 5) == 0);
    uint8_t r9[4] = { 0x2B, 0x0E, 4, 9 }; go(r9, 4); CHECK(EXC(2));
    uint8_t r3[4] = { 0x2B, 0x0E, 3, 0 }; go(r3, 4); CHECK(EXC(3));
}

/* MBAP framing */
static uint16_t frame(uint8_t *f, uint16_t tid, uint8_t unit, uint16_t a, uint16_t n) {
    uint8_t x[12] = { (uint8_t)(tid >> 8), (uint8_t)tid, 0, 0, 0, 6, unit, 3, (uint8_t)(a >> 8), (uint8_t)a, 0, (uint8_t)n };
    memcpy(f, x, 12); return 12;
}
static void t_mbtcp_framing(void) {
    setup();
    m_count = 9; scan();
    mbtcp_rx_t rx = { .n = 0 };
    uint8_t f[12], out[1100]; uint16_t used;
    frame(f, 0x1234, 7, 1, 1);
    /* byte by byte */
    int total = 0;
    for (int k = 0; k < 12; ++k) {
        int o = mbtcp_feed(&rx, f + k, 1, &used, out, sizeof out);
        CHECK_EQ(used, 1);
        if (k < 11) CHECK_EQ(o, 0); else total = o;
    }
    CHECK_EQ(total, 11);
    CHECK_EQ(out[0], 0x12); CHECK_EQ(out[1], 0x34); CHECK_EQ(out[5], 5); CHECK_EQ(out[6], 7);
    CHECK_EQ(out[7], 3); CHECK_EQ(out[8], 2); CHECK_EQ(out[10], 9);
    /* 40 requests in one buffer, output room for 3 answers at a time */
    static uint8_t many[40 * 12];
    for (int k = 0; k < 40; ++k) frame(many + 12 * k, (uint16_t)k, 1, 1, 1);
    int answers = 0, calls = 0; uint16_t off = 0; int last_tid = -1, in_order = 1;
    while (off < sizeof many && calls < 100) {
        int o = mbtcp_feed(&rx, many + off, (uint16_t)(sizeof many - off), &used, out, 3 * MBTCP_FRAME_MAX);
        CHECK(o >= 0);
        for (int p = 0; p < o; p += 11) { int tid = (out[p] << 8) | out[p + 1]; if (tid != last_tid + 1) in_order = 0; last_tid = tid; answers++; }
        off = (uint16_t)(off + used); calls++;
    }
    CHECK_EQ(answers, 40); CHECK(in_order); CHECK_EQ(rx.n, 0);
    /* exceptions travel inside a normal frame */
    frame(f, 1, 1, 2, 1);                                   /* unmapped -> exception 02 */
    int o = mbtcp_feed(&rx, f, 12, &used, out, sizeof out);
    CHECK_EQ(o, 9); CHECK_EQ(out[5], 3); CHECK_EQ(out[7], 0x83); CHECK_EQ(out[8], 2);
    /* not Modbus TCP */
    uint8_t bad[12]; frame(bad, 1, 1, 1, 1); bad[2] = 1;    /* protocol id 1 */
    rx.n = 0; CHECK_EQ(mbtcp_feed(&rx, bad, 12, &used, out, sizeof out), -1);
    frame(bad, 1, 1, 1, 1); bad[4] = 0x10;                 /* length 4102 */
    rx.n = 0; CHECK_EQ(mbtcp_feed(&rx, bad, 12, &used, out, sizeof out), -1);
    frame(bad, 1, 1, 1, 1); bad[5] = 1;                    /* length 1: no PDU */
    rx.n = 0; CHECK_EQ(mbtcp_feed(&rx, bad, 12, &used, out, sizeof out), -1);
}

/* the platform's own block at input registers 9000-9089, the same in every product (CIP-01.6, 15.1, 14.4) */
static void t_platform_block(void) {
    setup();
    mb_block_scan(5000);
    scan();
    rd(4, 9000, MB_BLOCK_REGS);
    CHECK_EQ(rl, 2 + 2 * MB_BLOCK_REGS);
    CHECK_EQ(reg(0), PLAT_MB_BLOCK_VERSION);
    CHECK_EQ(reg(1), 7);                                  /* the application's map version */
    uint16_t beat = reg(2);
    CHECK_EQ(reg(4), 0xFFFF);                             /* no supervisory request yet */
    CHECK_EQ(reg(11), 0x0001); CHECK_EQ(reg(12), 0x0203); /* firmware 1.2.3 */
    CHECK_EQ(reg(13), PLAT_API_VERSION);
    CHECK_EQ(reg(15), 5);                                 /* uptime 5 s */
    CHECK_EQ(reg(52), ('C' << 8) | 'E');                  /* "CEMS Supervisor" */
    CHECK_EQ(reg(68), ('A' << 8) | 'c');                  /* "Acme" */
    CHECK_EQ(reg(76), ('1' << 8) | '.');
    CHECK_EQ(reg(86), 0);                                 /* clock not synchronised (layout 2) */
    CHECK_EQ(reg(87), PLAT_CLK_SRC_NONE);
    /* each measurement's quality sits in the block beside the values (CIP-10.5) */
    plat_meas_q[0] = PLAT_Q_STALE; plat_meas_q[1] = PLAT_Q_RANGE;
    param_publish(); scan();
    rd(4, 9020, 2);
    CHECK_EQ(reg(0), PLAT_Q_STALE); CHECK_EQ(reg(1), PLAT_Q_RANGE);
    plat_meas_q[0] = plat_meas_q[1] = PLAT_Q_VALID;
    rd(4, 9000, MB_BLOCK_REGS);
    /* the heartbeat moves every 100 ms; the link age counts from the last request */
    param_comms_activity(PLAT_SRC_MODBUS, 5000);
    mb_block_scan(5050); mb_block_scan(5100); mb_block_scan(5150); mb_block_scan(7300);
    scan();
    rd(4, 9002, 3);
    CHECK_EQ(reg(0), (uint16_t)(beat + 2));
    CHECK_EQ(reg(2), 2);                                  /* 2.3 s since the request */
    /* an application entry inside the block is refused at start-up */
    mb_map_t clash[2] = { MB_ENTRY(MB_HOLDING, 9000, P_MODE, MBF_I16), MB_ENTRY(MB_INPUT, 9083, P_IN, MBF_U16) };
    CHECK_EQ(mb_init(clash, 2, NULL), 2);                 /* holding 9000 is fine, input 9083 is not */
    setup();
}

void test_modbus_all(void) {
    RUN(t_mbtcp_framing);
    RUN(t_map_validation);
    RUN(t_read_registers);
    RUN(t_read_errors);
    RUN(t_bits);
    RUN(t_write_single);
    RUN(t_write_multiple);
    RUN(t_text_and_bytes);
    RUN(t_permissions);
    RUN(t_busy);
    RUN(t_device_id);
    RUN(t_platform_block);
}
