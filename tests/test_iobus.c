/* test_iobus.c - RS-485 I/O bus: RTU framing, the master against simulated I/O boards
 * (connect, outputs and inputs, channel ranges, stale and safe outputs, a board that
 * stops answering and comes back, node restarts, faults, wrong type, settings conflicts,
 * lost replies, the node's comms timeout) and the simulated node's own rules. */
#include <string.h>
#include "tst.h"
#include "iobus.h"
#include "mb_rtu.h"
#include "iob_node_sim.h"
#include "bsp_host.h"
#include "param.h"
#include "plat_alarm.h"
#include "plat_config.h"
#include "plat_cmdq.h"

static iob_simbus_t bus;
static iob_node_t n1, n2, n3;

static void setup(void) {
    bsp_host_reset();
    bsp_host.ms = 10000;
    CHECK_EQ(param_init(NULL, 0), PERR_OK);
    param_set_defaults(PARAM_ALL);
    CHECK_EQ(alarm_init(NULL, 0, 0), 0);
    iob_reset();
    iob_simbus_init(&bus);
    iob_node_init(&n1, 1); iob_node_init(&n2, 2); iob_node_init(&n3, 3);
    iob_simbus_add(&bus, &n1); iob_simbus_add(&bus, &n2); iob_simbus_add(&bus, &n3);
    iob_init(&bus.port);
}

/* one scan (inputs in, outputs out) and one bus cycle, then time moves on */
static plat_io_in_t  last_in;
static plat_io_out_t cur_out;
static void step_ms(uint32_t ms, bool scan) {
    if (scan) {
        memset(&last_in, 0, sizeof last_in);
        iob_scan_in(&last_in, bsp_host.ms);
        alarm_scan(bsp_host.ms);
        iob_scan_out(&cur_out, bsp_host.ms);
    }
    iob_cycle(bsp_host.ms);
    bsp_host_advance_ms(ms);
}
static void run(uint32_t ms, bool scan) { for (uint32_t t = 0; t < ms; t += PLAT_IOB_PERIOD_MS) step_ms(PLAT_IOB_PERIOD_MS, scan); }
static iob_view_t view(unsigned b) { iob_view_t v; memset(&v, 0, sizeof v); CHECK(iob_board_view(b, &v)); return v; }
static bool alarm_on(uint16_t id) { alarm_view_t v; memset(&v, 0, sizeof v); return alarm_get(id, &v) && v.active; }

static void t_rtu_frames(void) {
    uint8_t f[MBR_FRAME_MAX];
    size_t n = mbr_req_read(f, 1, 0, 10);
    const uint8_t want[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD };   /* the textbook frame */
    CHECK_EQ(n, 8u);
    CHECK(memcmp(f, want, 8) == 0);
    CHECK_EQ(mbr_req_read(f, 0, 0, 1), 0u);              /* broadcast is not a read */
    CHECK_EQ(mbr_req_read(f, 248, 0, 1), 0u);
    CHECK_EQ(mbr_req_read(f, 1, 0, 126), 0u);

    /* replies */
    uint8_t rx[16] = { 0x01, 0x03, 0x04, 0x12, 0x34, 0xAB, 0xCD };
    uint16_t c = mbr_crc16(rx, 7); rx[7] = (uint8_t)c; rx[8] = (uint8_t)(c >> 8);
    n = mbr_req_read(f, 1, 16, 2);
    uint16_t r[2];
    CHECK_EQ(mbr_check_reply(f, n, rx, 9, r, 2, NULL), MBR_OK);
    CHECK_EQ(r[0], 0x1234); CHECK_EQ(r[1], 0xABCD);
    CHECK_EQ(mbr_check_reply(f, n, rx, 0, r, 2, NULL), MBR_NO_REPLY);
    rx[8] ^= 1u;
    CHECK_EQ(mbr_check_reply(f, n, rx, 9, r, 2, NULL), MBR_CRC);
    rx[8] ^= 1u;
    CHECK_EQ(mbr_check_reply(f, n, rx, 9, r, 1, NULL), MBR_BAD_FORMAT);   /* asked for a different count */
    uint8_t ex[5] = { 0x01, 0x83, 0x02 }; c = mbr_crc16(ex, 3); ex[3] = (uint8_t)c; ex[4] = (uint8_t)(c >> 8);
    uint8_t code = 0;
    CHECK_EQ(mbr_check_reply(f, n, ex, 5, r, 2, &code), MBR_EXCEPTION);
    CHECK_EQ(code, 2);
    ex[0] = 2; c = mbr_crc16(ex, 3); ex[3] = (uint8_t)c; ex[4] = (uint8_t)(c >> 8);
    CHECK_EQ(mbr_check_reply(f, n, ex, 5, r, 2, &code), MBR_WRONG_ADDR);
    CHECK(strcmp(mbr_result_name(MBR_CRC), "CRC error") == 0);
}

static void t_node_rules(void) {
    iob_node_t n; iob_node_init(&n, 5);
    uint8_t f[MBR_FRAME_MAX], rx[MBR_FRAME_MAX];
    uint16_t r[IOB_R_IDENT_N];
    size_t len = mbr_req_read(f, 5, IOB_R_MAGIC, IOB_R_IDENT_N);
    uint16_t got = iob_node_frame(&n, 0, f, (uint16_t)len, rx, sizeof rx);
    CHECK_EQ(mbr_check_reply(f, len, rx, got, r, IOB_R_IDENT_N, NULL), MBR_OK);
    CHECK_EQ(r[0], IOB_MAGIC); CHECK_EQ(r[1], IOB_MAP_VERSION);
    /* other addresses and bad CRCs are ignored; reads past the map are refused */
    len = mbr_req_read(f, 6, 0, 1);
    CHECK_EQ(iob_node_frame(&n, 0, f, (uint16_t)len, rx, sizeof rx), 0u);
    len = mbr_req_read(f, 5, 0, 1); f[len - 1] ^= 1u;
    CHECK_EQ(iob_node_frame(&n, 0, f, (uint16_t)len, rx, sizeof rx), 0u);
    CHECK_EQ(n.rx_errors, 1u);
    len = mbr_req_read(f, 5, 70, 20);
    got = iob_node_frame(&n, 0, f, (uint16_t)len, rx, sizeof rx);
    uint8_t code = 0;
    CHECK_EQ(mbr_check_reply(f, len, rx, got, r, 20, &code), MBR_EXCEPTION);
    CHECK_EQ(code, 2);
    /* status registers are read-only; a bad value changes nothing */
    len = mbr_req_write1(f, 5, IOB_R_STATUS, 1);
    got = iob_node_frame(&n, 0, f, (uint16_t)len, rx, sizeof rx);
    CHECK_EQ(mbr_check_reply(f, len, rx, got, NULL, 0, &code), MBR_EXCEPTION); CHECK_EQ(code, 2);
    uint16_t cfg[4] = { 6001, 3, 0, 0xFF };               /* hold time over 10 min */
    len = mbr_req_write(f, 5, IOB_R_HOLD_DS, cfg, 4);
    got = iob_node_frame(&n, 0, f, (uint16_t)len, rx, sizeof rx);
    CHECK_EQ(mbr_check_reply(f, len, rx, got, NULL, 0, &code), MBR_EXCEPTION); CHECK_EQ(code, 3);
    CHECK_EQ(n.hold_ds, 3000u);
    CHECK(n.config_needed);
    /* gate opens on the first output write and closes after the timeout */
    CHECK(!(iob_node_reg(&n, IOB_R_STATUS) & IOB_ST_OUT_ENABLED));
    uint16_t w[3] = { 0x0101, 1, 7 };
    len = mbr_req_rw(f, 5, IOB_R_STATUS, IOB_R_STATUS_N, 0, w, 3);
    uint16_t st[IOB_R_STATUS_N];
    got = iob_node_frame(&n, 100, f, (uint16_t)len, rx, sizeof rx);
    CHECK_EQ(mbr_check_reply(f, len, rx, got, st, IOB_R_STATUS_N, NULL), MBR_OK);
    CHECK(st[0] & IOB_ST_OUT_ENABLED);
    CHECK_EQ(st[IOB_R_OUT_APPLIED - IOB_R_STATUS], 0x0101);
    CHECK_EQ(st[IOB_R_CMD_ECHO - IOB_R_STATUS], 7);        /* write before read */
    iob_node_tick(&n, 300099); CHECK(n.gate);       /* holds for 5 minutes by default */
    iob_node_tick(&n, 300100); CHECK(!n.gate);
    CHECK_EQ(iob_node_reg(&n, IOB_R_OUT_APPLIED), 0u);
    CHECK(iob_node_reg(&n, IOB_R_STATUS) & IOB_ST_TIMEOUT);
    /* unknown function */
    f[0] = 5; f[1] = 0x2B; f[2] = 0x0E; f[3] = 1; uint16_t c = mbr_crc16(f, 4); f[4] = (uint8_t)c; f[5] = (uint8_t)(c >> 8);
    got = iob_node_frame(&n, 300200, f, 6, rx, sizeof rx);
    CHECK_EQ(got, 5u); CHECK_EQ(rx[1], 0xABu); CHECK_EQ(rx[2], 1u);
}

static void t_off_by_default(void) {
    setup();
    run(200, true);
    CHECK_EQ(bus.requests, 0u);                            /* no board enabled: the bus stays quiet */
    CHECK_EQ(view(0).state, IOB_S_OFF);
    CHECK_EQ(last_in.di, 0u);
    CHECK_EQ(last_in.do_fault, 0u);
    CHECK(!alarm_on(ALARM_ID_IOB_LOST));
    CHECK(param_find(PARAM_ID_IOB_FIRST) != NULL);
    CHECK(param_find(PARAM_ID_IOB_LIVE_FIRST + 5) != NULL);
    CHECK_EQ(iob_tags[1].first_do, 19u);
    CHECK_EQ(iob_tags[2].first_di, 17u);
}

static void t_connect_and_io(void) {
    setup();
    iob_tags[0].enable = true;
    n1.inputs = 0x05;
    memset(&cur_out, 0, sizeof cur_out);
    plat_do_set(&cur_out, 0, true);      /* bank A output 1 */
    plat_do_set(&cur_out, 9, true);      /* bank B output 10 */
    plat_do_set(&cur_out, 16, true);     /* relay 1 */
    run(20, true);                       /* settings reach the bus task */
    run(20, true);
    iob_view_t v = view(0);
    CHECK_EQ(v.state, IOB_S_RUN);
    CHECK(v.status & IOBS_ONLINE);
    CHECK(!n1.config_needed);
    CHECK_EQ(n1.hold_ds, 3000u); CHECK_EQ(n1.ol_en, 0xFFu); CHECK_EQ(n1.wb_en, 0u); CHECK_EQ(n1.filter_ms, 8u);
    CHECK_EQ(n1.out_cmd, 0x0201u);
    CHECK_EQ(n1.relay_cmd, 1u);
    CHECK_EQ(v.outputs, 0x10201u);
    CHECK_EQ(v.fw_version, 0x0100u);
    run(20, true);
    CHECK_EQ(last_in.di, 0x05u);
    CHECK_EQ(last_in.di_fault, 0u);
    CHECK_EQ(last_in.do_fault, 0u);
    CHECK_EQ(iob_tags[0].inputs, 0x05u);
    CHECK(iob_tags[0].status & IOBS_GATE_OPEN);
    /* one request per cycle while running */
    uint32_t r0 = bus.requests;
    run(200, true);
    CHECK_EQ(bus.requests - r0, 10u);
    CHECK_EQ(view(0).errors, 0u);
    CHECK(!alarm_on(ALARM_ID_IOB_LOST));
}

static void t_channel_ranges(void) {
    setup();
    iob_tags[1].enable = true;           /* board 2 only: outputs 19-36, inputs 9-16 */
    n2.inputs = 0x81;
    memset(&cur_out, 0, sizeof cur_out);
    plat_do_set(&cur_out, 18, true);     /* channel 19 = board 2 output 1 */
    plat_do_set(&cur_out, 35, true);     /* channel 36 = board 2 relay 2 */
    plat_do_set(&cur_out, 0, true);      /* channel 1: not board 2's */
    run(80, true);
    CHECK_EQ(view(1).state, IOB_S_RUN);
    CHECK_EQ(n2.out_cmd, 0x0001u);
    CHECK_EQ(n2.relay_cmd, 2u);
    CHECK_EQ(n1.frames > 0, true);       /* board 1 sees the traffic but is not addressed */
    CHECK_EQ(n1.replies, 0u);
    CHECK_EQ(last_in.di, (uint64_t)0x81u << 8);
    /* main-board inputs outside the range are kept; inside it the board wins */
    bsp_host.in.di = 0x3u | (1u << 9);
    plat_io_in_t in; memset(&in, 0, sizeof in);
    plat_hw_io_read(&in);
    CHECK_EQ(in.di, 0x3u | ((uint64_t)0x81u << 8));
}

static void t_stale_and_safe(void) {
    setup();
    iob_tags[0].enable = true;
    memset(&cur_out, 0, sizeof cur_out);
    plat_do_set(&cur_out, 3, true);
    run(60, true);
    CHECK_EQ(n1.out_cmd, 0x0008u);
    run(PLAT_IOB_STALE_MS - 40, false);  /* scan stopped, still within the limit */
    CHECK_EQ(n1.out_cmd, 0x0008u);
    run(60, false);
    CHECK_EQ(n1.out_cmd, 0u);            /* stale: all off, but the link stays alive */
    CHECK(n1.gate);
    run(40, true);
    CHECK_EQ(n1.out_cmd, 0x0008u);       /* the scan is back */
    iob_safe_state();
    run(40, true);
    CHECK_EQ(n1.out_cmd, 0u);
    CHECK_EQ(n1.relay_cmd, 0u);
    plat_io_safe_state();                /* the BSP's pins too */
    CHECK(bsp_host.safe_states > 0);
}

/* the simulated node's address handshake, commissioning address and serial (iobus.h, node FW-61/62):
 * the main board's address change and "add I/O board" pages are built against these rules */
static uint16_t sim_x(iob_node_t *n, uint32_t now, const uint8_t *f, size_t len, uint8_t *rx, uint16_t *r, uint16_t nr, uint8_t *code) {
    uint16_t got = iob_node_frame(n, now, f, (uint16_t)len, rx, MBR_FRAME_MAX);
    *code = 0;
    return (uint16_t)mbr_check_reply(f, len, rx, got, r, nr, code);
}
static void t_node_cfg(void) {
    iob_node_t n; iob_node_init(&n, 0);                    /* a new board: no address */
    memcpy(n.serial, "CIO-26-00042", 12);
    uint8_t f[MBR_FRAME_MAX], rx[MBR_FRAME_MAX], code;
    uint16_t r[8];
    size_t len = mbr_req_read(f, IOB_COMMISSION_ADDR, IOB_R_UID, 6);
    CHECK_EQ(sim_x(&n, 0, f, len, rx, r, 6, &code), MBR_OK);
    CHECK_EQ(r[0], n.uid[0]);
    len = mbr_req_read(f, IOB_COMMISSION_ADDR, IOB_R_SERIAL, IOB_SERIAL_REGS);
    CHECK_EQ(sim_x(&n, 0, f, len, rx, r, 8, &code), MBR_OK);
    CHECK_EQ(r[0], 0x4349u); CHECK_EQ(r[6], 0u);
    uint16_t w[3] = { 1, 0, 1 };
    len = mbr_req_rw(f, IOB_COMMISSION_ADDR, IOB_R_STATUS, 1, 0, w, 3);
    CHECK_EQ(sim_x(&n, 0, f, len, rx, r, 1, &code), MBR_EXCEPTION); CHECK_EQ(code, 4);   /* nothing switches */
    /* unlock, challenge, commit */
    uint16_t u[7] = { IOB_CFG_UNLOCK_KEY, n.uid[0], n.uid[1], n.uid[2], n.uid[3], n.uid[4], n.uid[5] };
    len = mbr_req_write(f, IOB_COMMISSION_ADDR, IOB_R_CFG_UNLOCK, u, 7);
    CHECK_EQ(sim_x(&n, 10, f, len, rx, NULL, 0, &code), MBR_OK);
    len = mbr_req_read(f, IOB_COMMISSION_ADDR, IOB_R_CFG_UNLOCK, 1);
    CHECK_EQ(sim_x(&n, 20, f, len, rx, r, 1, &code), MBR_OK);
    uint16_t ch = r[0];
    CHECK(ch != 0u);
    uint32_t chk = iob_cfg_check(n.uid, ch, 4);
    uint16_t c[4] = { 4, ch, (uint16_t)(chk >> 16), (uint16_t)chk };
    len = mbr_req_write(f, IOB_COMMISSION_ADDR, IOB_R_CFG_ADDR, c, 4);
    CHECK_EQ(sim_x(&n, 30, f, len, rx, NULL, 0, &code), MBR_OK);
    CHECK_EQ(n.address, 4u);
    /* a wrong check is refused and ends the unlock; a direct write of 38 is refused */
    u[0] = IOB_CFG_UNLOCK_KEY;
    len = mbr_req_write(f, 4, IOB_R_CFG_UNLOCK, u, 7);
    CHECK_EQ(sim_x(&n, 40, f, len, rx, NULL, 0, &code), MBR_OK);
    len = mbr_req_read(f, 4, IOB_R_CFG_UNLOCK, 1);
    CHECK_EQ(sim_x(&n, 50, f, len, rx, r, 1, &code), MBR_OK);
    ch = r[0];
    chk = iob_cfg_check(n.uid, ch, 7) ^ 1u;
    uint16_t c2[4] = { 7, ch, (uint16_t)(chk >> 16), (uint16_t)chk };
    len = mbr_req_write(f, 4, IOB_R_CFG_ADDR, c2, 4);
    CHECK_EQ(sim_x(&n, 60, f, len, rx, NULL, 0, &code), MBR_EXCEPTION); CHECK_EQ(code, 4);
    CHECK_EQ(n.address, 4u);
    CHECK_EQ(n.cfg_challenge, 0u);
    len = mbr_req_write1(f, 4, IOB_R_NODE_ADDR, 7);
    CHECK_EQ(sim_x(&n, 70, f, len, rx, NULL, 0, &code), MBR_EXCEPTION); CHECK_EQ(code, 2);
    uint16_t sn[8] = { 0x4142 };
    len = mbr_req_write(f, 4, IOB_R_SERIAL, sn, 8);
    CHECK_EQ(sim_x(&n, 80, f, len, rx, NULL, 0, &code), MBR_EXCEPTION); CHECK_EQ(code, 2);   /* USB only */
}

static void t_lost_and_back(void) {
    setup();
    iob_tags[0].enable = true;
    n1.inputs = 0x0F;
    memset(&cur_out, 0, sizeof cur_out);
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    n1.powered = false;
    run(900, true);                      /* inside PLAT_IOB_LOST_MS: last inputs held */
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK_EQ(last_in.di, 0x0Fu);
    run(200, true);
    iob_view_t v = view(0);
    CHECK_EQ(v.state, IOB_S_LOST);
    CHECK(v.status & IOBS_LOST);
    run(20, true);
    CHECK_EQ(last_in.di, 0u);            /* not responding: inputs 0, every channel faulty */
    CHECK_EQ(last_in.di_fault, 0xFFu);
    CHECK_EQ(last_in.do_fault, 0x3FFFFu);
    CHECK(alarm_on(ALARM_ID_IOB_LOST));
    CHECK(v.errors > 10u);
    uint32_t r0 = bus.requests;
    run(2000, true);                     /* retried once a second, not every cycle */
    CHECK(bus.requests - r0 <= 3u);
    n1.powered = true;
    iob_node_restart(&n1, false, bsp_host.ms);
    run(1100, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK(!n1.config_needed);
    run(20, true);
    CHECK_EQ(last_in.di, 0x0Fu);
    CHECK_EQ(last_in.do_fault, 0u);
    run(2000, true);
    CHECK(alarm_on(ALARM_ID_IOB_LOST));  /* latched: stays until acknowledged */
    alarm_ack(ALARM_ID_IOB_LOST, PLAT_SRC_LOCAL, 0);
    run(40, true);
    CHECK(!alarm_on(ALARM_ID_IOB_LOST));
}

static bool alarm_unacked(uint16_t id) { alarm_view_t v; memset(&v, 0, sizeof v); return alarm_get(id, &v) && v.unacked; }

/* the node's latched comms fault (node requirements FW-58): neither end clears it by itself */
static void t_comms_latch(void) {
    setup();
    iob_tags[0].enable = true;
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK(!n1.comms_lost);
    /* 1: the link breaks for 2 s and comes back: both ends hold the fault until acknowledged */
    n1.powered = false;
    run(2000, true);
    CHECK_EQ(view(0).state, IOB_S_LOST);
    CHECK(alarm_on(ALARM_ID_IOB_LOST));
    n1.powered = true;
    run(1500, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK(n1.comms_lost);                        /* the board saw it too */
    CHECK(view(0).status & IOBS_COMMS_LATCHED);
    run(5000, true);
    CHECK(n1.comms_lost);                        /* not cleared by healthy traffic */
    CHECK(alarm_on(ALARM_ID_IOB_LOST));
    CHECK(alarm_unacked(ALARM_ID_IOB_LOST));
    char lines[4][IOB_LINE];
    iob_view_t v = view(0);
    CHECK(iob_fault_lines(&v, 0xFF, 0, lines, 4) >= 1);
    CHECK(strcmp(lines[0], "Link was lost: acknowledge") == 0);
    alarm_ack(ALARM_ID_IOB_LOST, PLAT_SRC_LOCAL, 0);
    run(100, true);
    CHECK(!n1.comms_lost);                       /* cleared on the board by the master */
    CHECK(!(view(0).status & IOBS_COMMS_LATCHED));
    run(40, true);
    CHECK(!alarm_on(ALARM_ID_IOB_LOST));
    /* 2: acknowledged while still lost: cleared on both ends when the link returns */
    n1.powered = false;
    run(2000, true);
    CHECK(alarm_on(ALARM_ID_IOB_LOST));
    alarm_ack(ALARM_ID_IOB_LOST, PLAT_SRC_LOCAL, 0);
    run(100, true);
    CHECK(alarm_on(ALARM_ID_IOB_LOST));          /* still lost */
    n1.powered = true;
    run(1500, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK(!n1.comms_lost);
    run(40, true);
    CHECK(!alarm_on(ALARM_ID_IOB_LOST));
    /* 3: only the board saw it (the main board stopped polling, as in its own restart):
     * the board's bit raises IOB_LOST, which then waits for an acknowledge as well */
    iob_tags[0].enable = false;
    run(2000, true);
    iob_tags[0].enable = true;
    run(1500, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK(n1.comms_lost);
    CHECK(alarm_on(ALARM_ID_IOB_LOST));
    CHECK(alarm_unacked(ALARM_ID_IOB_LOST));
    run(3000, true);
    CHECK(n1.comms_lost);
    alarm_ack(ALARM_ID_IOB_LOST, PLAT_SRC_LOCAL, 0);
    run(100, true);
    CHECK(!n1.comms_lost);
    run(40, true);
    CHECK(!alarm_on(ALARM_ID_IOB_LOST));
}

/* a short opening of an alarm contact between two polls still reaches the image (FW-33) */
static void t_input_pulse(void) {
    setup();
    iob_tags[0].enable = true;
    n1.inputs = 0x01;                            /* healthy contact closed */
    run(60, true);
    CHECK_EQ(last_in.di & 1u, 1u);
    n1.inputs = 0x00;                            /* opens for 3 ms, between two polls */
    for (uint32_t t = 1; t <= 3; t++) iob_node_tick(&n1, bsp_host.ms + t);
    n1.inputs = 0x01;
    for (uint32_t t = 4; t <= 6; t++) iob_node_tick(&n1, bsp_host.ms + t);
    run(40, true);                               /* the scan sees each poll one cycle later */
    CHECK_EQ(last_in.di & 1u, 0u);               /* seen open once */
    run(20, true);
    CHECK_EQ(last_in.di & 1u, 1u);
}

static void t_node_restart(void) {
    setup();
    iob_tags[0].enable = true;
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    iob_node_restart(&n1, true, bsp_host.ms);   /* watchdog reset: settings lost */
    iob_tags[0].wb_mask = 0x0F;                 /* (a settings change on the way) */
    run(60, true);
    CHECK(!n1.config_needed);                   /* settings sent again */
    CHECK_EQ(n1.wb_en, 0x0Fu);
    CHECK(!n1.wdg_latched);                     /* latch cleared by the master */
    CHECK(view(0).status & IOBS_WDG_SEEN);
    run(20, true);
    CHECK(alarm_on(ALARM_ID_IOB_NODE_RESET));
    run(200, true);
    CHECK(alarm_on(ALARM_ID_IOB_NODE_RESET));   /* latched until acknowledged */
}

static void t_faults(void) {
    setup();
    iob_tags[0].enable = true;
    iob_tags[0].ol_mask = 0xFB;                 /* no open-circuit check on output 3 (no valve fitted) */
    iob_tags[0].wb_mask = 0x01;
    run(60, true);
    n1.a_open = 0x06;                           /* outputs 2 and 3 open */
    n1.a_overtemp = 0x80;                       /* output 8 too hot */
    n1.wirebreak = 0x03;                        /* inputs 1 and 2 (only 1 checked) */
    run(40, true);
    iob_view_t v = view(0);
    CHECK_EQ(v.out_faults, 0x82u);
    CHECK_EQ(v.in_faults, 0x01u);
    CHECK_EQ(last_in.do_fault, 0x82u);
    CHECK_EQ(last_in.di_fault, 0x01u);
    CHECK(plat_do_fault(&last_in, 1)); CHECK(!plat_do_fault(&last_in, 2));
    CHECK(plat_di_fault(&last_in, 0));
    run(1200, true);
    CHECK(alarm_on(ALARM_ID_IOB_OUT_FAULT));
    CHECK(alarm_on(ALARM_ID_IOB_IN_FAULT));
    CHECK(!alarm_on(ALARM_ID_IOB_SUPPLY));
    n1.b_fault = IOB_BF_FAULT;
    run(40, true);
    CHECK_EQ(view(0).out_faults, 0xFF82u);      /* bank B: one line for all 8 */
    n1.a_open = n1.a_overtemp = n1.wirebreak = 0; n1.b_fault = 0;
    n1.field_a = false;                         /* bank A and the inputs lose their supply */
    run(40, true);
    v = view(0);
    CHECK_EQ(v.out_faults, 0x00FFu);
    CHECK_EQ(v.in_faults, 0xFFu);
    CHECK(!(v.status & IOBS_FIELD_A_OK));
    run(600, true);
    CHECK(alarm_on(ALARM_ID_IOB_SUPPLY));
    n1.field_a = true;
    run(1500, true);
    CHECK_EQ(view(0).out_faults, 0u);
    CHECK(!alarm_on(ALARM_ID_IOB_SUPPLY));
    CHECK(!alarm_on(ALARM_ID_IOB_OUT_FAULT));
    CHECK_EQ(iob_tags[0].out_faults, 0u);
}

static void t_wrong_type_and_conflicts(void) {
    setup();
    n1.magic = 0x1234;                          /* some other Modbus device at address 1 */
    iob_tags[0].enable = true;
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_WRONG_TYPE);
    CHECK_EQ(n1.writes0, 0u);                   /* nothing written to a stranger */
    run(20, true);
    CHECK(alarm_on(ALARM_ID_IOB_CONFIG));
    n1.magic = IOB_MAGIC;
    run(1100, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    run(20, true);
    CHECK(!alarm_on(ALARM_ID_IOB_CONFIG));

    iob_tags[1].enable = true;
    iob_tags[1].first_do = 10;                  /* overlaps board 1's outputs 1-18 */
    run(40, true);
    CHECK_EQ(view(1).state, IOB_S_BAD_SETTINGS);
    CHECK_EQ(view(0).state, IOB_S_RUN);         /* the first board keeps running */
    CHECK_EQ(n2.replies, 0u);
    run(20, true);
    CHECK(alarm_on(ALARM_ID_IOB_CONFIG));
    iob_tags[1].first_do = 19;
    iob_tags[1].address = 1;                    /* same address as board 1 */
    run(40, true);
    CHECK_EQ(view(1).state, IOB_S_BAD_SETTINGS);
    iob_tags[1].address = 2;
    iob_tags[2].enable = true;
    iob_tags[2].first_do = 50;                  /* 50 + 17 > 64 */
    run(60, true);
    CHECK_EQ(view(1).state, IOB_S_RUN);
    CHECK_EQ(view(2).state, IOB_S_BAD_SETTINGS);
    iob_tags[2].first_do = 37;
    run(60, true);
    CHECK_EQ(view(2).state, IOB_S_RUN);
    uint32_t r0 = bus.requests;
    run(200, true);
    CHECK_EQ(bus.requests - r0, 30u);           /* three boards, one request each per cycle */
}

static void t_lost_replies(void) {
    setup();
    iob_tags[0].enable = true;
    memset(&cur_out, 0, sizeof cur_out);
    plat_do_set(&cur_out, 2, true);
    run(60, true);
    bus.drop_next = 3; bus.corrupt_next = 2;
    run(200, true);
    iob_view_t v = view(0);
    CHECK_EQ(v.state, IOB_S_RUN);               /* a few bad frames do not drop the board */
    CHECK_EQ(v.errors, 5u);
    CHECK_EQ(v.last_error, MBR_CRC);
    CHECK_EQ(n1.out_cmd, 0x0004u);
    CHECK(n1.gate);
}

static void t_node_timeout(void) {
    setup();
    iob_tags[0].enable = true;
    memset(&cur_out, 0, sizeof cur_out);
    plat_do_set(&cur_out, 0, true);
    run(60, true);
    CHECK(n1.gate);
    iob_hold_s = 2;                             /* a short hold, to see it run out */
    run(60, true);
    CHECK_EQ(n1.hold_ds, 20u);
    bsp_host_advance_ms(1900);                  /* the master falls silent (main board restarting) */
    iob_node_tick(&n1, bsp_host.ms);
    CHECK(n1.gate);                             /* still holding */
    bsp_host_advance_ms(200);
    iob_node_tick(&n1, bsp_host.ms);
    CHECK(!n1.gate);                            /* the node turned its outputs off */
    CHECK_EQ(iob_node_reg(&n1, IOB_R_OUT_APPLIED), 0u);
    run(60, true);
    CHECK(n1.gate);                             /* the next write re-opens it */
    CHECK(view(0).status & IOBS_TIMEOUT_SEEN);
    CHECK(!n1.timeout_latched);                 /* cleared by the master once seen */
    iob_hold_s = 300;
    run(60, true);
    CHECK_EQ(n1.hold_ds, 3000u);
}

static void t_no_port(void) {
    setup();
    iob_init(NULL);
    iob_tags[0].enable = true;
    run(1100, true);
    CHECK_EQ(view(0).state, IOB_S_LOST);
    iob_init(&bus.port);
}

void test_iobus_all(void) {
    t_node_cfg();
    t_comms_latch();
    t_input_pulse();
    RUN(t_rtu_frames);
    RUN(t_node_rules);
    RUN(t_off_by_default);
    RUN(t_connect_and_io);
    RUN(t_channel_ranges);
    RUN(t_stale_and_safe);
    RUN(t_lost_and_back);
    RUN(t_node_restart);
    RUN(t_faults);
    RUN(t_wrong_type_and_conflicts);
    RUN(t_lost_replies);
    RUN(t_node_timeout);
    RUN(t_no_port);
    iob_reset();
    memset(iob_tags, 0, sizeof iob_tags);
    param_set_defaults(PARAM_ALL);
}
