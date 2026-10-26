/* test_iobus.c - the I/O bus on both links (RS-485 and Ethernet): RTU framing, the master against simulated I/O boards
 * (connect, outputs and inputs, channel ranges, stale and safe outputs, a board that
 * stops answering and comes back, node restarts, faults, wrong type, settings conflicts,
 * lost replies, the node's comms timeout) and the simulated node's own rules.
 * Requirements proved here: CIP-13.2, 13.3, 15.3, 26.4. */
#include <string.h>
#include "tst.h"
#include "iobus.h"
#include "mb_rtu.h"
#include "iob_node_sim.h"
#include "iob_w5500.h"
#include "w5500_sim.h"
#include "bsp_host.h"
#include "param.h"
#include "plat_alarm.h"
#include "plat_config.h"
#include "plat_cmdq.h"

static iob_simbus_t bus;
static iob_simnet_t net;
static iob_node_t n1, n2, n3;
static w5500_sim_t chip;                /* the I/O port: a simulated W5500 on the same simulated network */
static iob_w5500_t io;
static uint8_t cur_link;                /* the run of the tests: RS-485, Ethernet on the main port, the I/O port */
#define SUP_IP   0xC0A80132u            /* the supervisor, 192.168.1.50 (on the I/O port: its own IP, set the same) */
#define ETH      (cur_link != IOB_LINK_RS485)
#define REQUESTS (ETH ? net.requests : bus.requests)

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
    iob_simnet_init(&net, SUP_IP);
    iob_simnet_add(&net, &n1); iob_simnet_add(&net, &n2); iob_simnet_add(&net, &n3);
    w5500_sim_init(&chip);
    w5500_sim_simnet(&chip, &net);
    chip.bus.ms = bsp_ms;
    static const uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x57 };
    iob_w5500_init(&io, &chip.bus, mac);
    iob_init(&bus.port);
    iob_init_eth(&net.port, &net.disc);
    iob_init_ioport(&io.port, &io.disc);
    iob_link = cur_link;
    iob_port_ip = SUP_IP;
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
    len = mbr_req_read(f, 5, 100, 10);                     /* map 2 ends at 106 */
    got = iob_node_frame(&n, 0, f, (uint16_t)len, rx, sizeof rx);
    uint8_t code = 0;
    CHECK_EQ(mbr_check_reply(f, len, rx, got, r, 10, &code), MBR_EXCEPTION);
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
    CHECK_EQ(REQUESTS, 0u);                            /* no board enabled: the bus stays quiet */
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
    uint32_t r0 = REQUESTS;
    run(200, true);
    CHECK_EQ(REQUESTS - r0, 10u);
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
    CHECK_EQ(n1.frames > 0, !ETH);       /* on RS-485 board 1 sees the traffic but is not addressed */
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

/* the simulated node's address change, commissioning address and serial (iobus.h, node FW-61/62):
 * the main board's address change, scan and "add I/O board" pages are built against these rules */
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
    /* the address change: one FC16 of 40-42 = key, address, address XOR 0xFFFF */
    uint16_t bad[3] = { IOB_ADDR_SET_KEY, 4, 4 };                 /* wrong complement */
    len = mbr_req_write(f, IOB_COMMISSION_ADDR, IOB_R_ADDR_SET, bad, 3);
    CHECK_EQ(sim_x(&n, 10, f, len, rx, NULL, 0, &code), MBR_EXCEPTION); CHECK_EQ(code, 3);
    CHECK_EQ(n.address, 0u);
    uint16_t c[3] = { IOB_ADDR_SET_KEY, 4, 0xFFFBu };
    len = mbr_req_write(f, IOB_COMMISSION_ADDR, IOB_R_ADDR_SET, c, 3);
    CHECK_EQ(sim_x(&n, 30, f, len, rx, NULL, 0, &code), MBR_OK);
    CHECK_EQ(rx[0], IOB_COMMISSION_ADDR);                  /* the reply from the old address */
    CHECK_EQ(n.address, 4u);
    /* the key alone, or a wrong key: refused */
    len = mbr_req_write1(f, 4, IOB_R_ADDR_SET, IOB_ADDR_SET_KEY);
    CHECK_EQ(sim_x(&n, 40, f, len, rx, NULL, 0, &code), MBR_EXCEPTION); CHECK_EQ(code, 2);
    uint16_t k[3] = { 0x1234, 7, 0xFFF8u };
    len = mbr_req_write(f, 4, IOB_R_ADDR_SET, k, 3);
    CHECK_EQ(sim_x(&n, 50, f, len, rx, NULL, 0, &code), MBR_EXCEPTION); CHECK_EQ(code, 3);
    CHECK_EQ(n.address, 4u);
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
    uint32_t r0 = REQUESTS;
    run(2000, true);                     /* retried once a second, not every cycle */
    CHECK(REQUESTS - r0 <= 3u);
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
    if (ETH) iob_tags[1].ip = iob_tags[0].ip;   /* same IP as board 1 */
    else iob_tags[1].address = 1;               /* same address as board 1 */
    run(40, true);
    CHECK_EQ(view(1).state, IOB_S_BAD_SETTINGS);
    iob_tags[1].ip = 0xC0A8013Eu;
    plat_val_t a = { .i = IOB_ADDR_MAX };       /* the setting stops below the reserved addresses */
    CHECK_EQ(param_check(PARAM_ID_IOB_FIRST + 11, PLAT_VAL_I32, a, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);
    a.i = IOB_COMMISSION_ADDR;
    CHECK_EQ(param_check(PARAM_ID_IOB_FIRST + 11, PLAT_VAL_I32, a, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_RANGE);
    iob_tags[1].address = IOB_COMMISSION_ADDR;  /* reserved: a board without an address */
    run(40, true);
    CHECK_EQ(view(1).state, IOB_S_BAD_SETTINGS);
    iob_tags[1].address = IOB_USB_ADDR;         /* reserved: the node's USB port */
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
    uint32_t r0 = REQUESTS;
    run(200, true);
    CHECK_EQ(REQUESTS - r0, 30u);           /* three boards, one request each per cycle */
}

/* commissioning from the web page: find a new board at 246, give it an address, never a used one */
static iob_cm_view_t cm_wait(void) {
    iob_cm_view_t v;
    for (int i = 0; i < 10; ++i) { run(20, true); iob_cm_view(&v); if (v.result != IOB_CM_BUSY) break; }
    return v;
}
static void t_commission(void) {
    if (ETH) return;                            /* RS-485 only: see t_eth_commission */
    setup();
    iob_node_init(&n3, 0);                      /* a new board: no address */
    memcpy(n3.serial, "CIO-26-00042", 12);
    iob_tags[0].enable = iob_tags[1].enable = iob_tags[2].enable = true;
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK(view(2).state != IOB_S_RUN);         /* nothing answers at 3 yet */

    CHECK(iob_cm_find(IOB_COMMISSION_ADDR));
    CHECK(!iob_cm_find(IOB_COMMISSION_ADDR));   /* one request at a time */
    CHECK(!iob_cm_assign(IOB_COMMISSION_ADDR, 3));
    iob_cm_view_t v = cm_wait();
    CHECK_EQ(v.result, IOB_CM_OK);
    CHECK(v.found);
    CHECK_EQ(v.uid[0], n3.uid[0]); CHECK_EQ(v.uid[5], n3.uid[5]);
    CHECK(!strcmp(v.serial, "CIO-26-00042"));
    CHECK_EQ(v.fw_version, n3.fw_version);
    CHECK_EQ(n3.writes0, 0u);                   /* nothing switched on the new board */

    CHECK(!iob_cm_assign(IOB_COMMISSION_ADDR, IOB_COMMISSION_ADDR));   /* out of range */
    CHECK(!iob_cm_assign(IOB_COMMISSION_ADDR, 0));
    CHECK(!iob_cm_assign(3, 3));
    CHECK(iob_cm_assign(IOB_COMMISSION_ADDR, 1));                       /* board 1 is at 1 */
    v = cm_wait();
    CHECK_EQ(v.result, IOB_CM_IN_USE);
    CHECK_EQ(n3.address, 0u);
    CHECK_EQ(view(0).state, IOB_S_RUN);

    CHECK(iob_cm_assign(IOB_COMMISSION_ADDR, 3));
    v = cm_wait();
    CHECK_EQ(v.result, IOB_CM_OK);
    CHECK_EQ(n3.address, 3u);
    CHECK(!strcmp(v.serial, "CIO-26-00042"));   /* read back at the new address */
    run(PLAT_IOB_RETRY_MS + 100, true);
    CHECK_EQ(view(2).state, IOB_S_RUN);         /* board 3 comes online by itself */
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK_EQ(view(1).state, IOB_S_RUN);

    CHECK(iob_cm_find(IOB_COMMISSION_ADDR));    /* no new board left */
    CHECK_EQ(cm_wait().result, IOB_CM_NO_REPLY);
    CHECK(iob_cm_assign(200, 9));               /* nothing at 200 */
    CHECK_EQ(cm_wait().result, IOB_CM_NO_REPLY);
    n1.magic = 0x1234;                          /* some other Modbus device at 1 */
    CHECK(iob_cm_find(1));
    CHECK_EQ(cm_wait().result, IOB_CM_WRONG_TYPE);
    n1.magic = IOB_MAGIC;
    iob_cm_view(&v);
    CHECK_EQ(v.seq, 6u);
}

static void t_lost_replies(void) {
    setup();
    iob_tags[0].enable = true;
    memset(&cur_out, 0, sizeof cur_out);
    plat_do_set(&cur_out, 2, true);
    run(60, true);
    if (ETH) net.drop_next = 5;                 /* TCP has no CRC errors: lost replies only */
    else { bus.drop_next = 3; bus.corrupt_next = 2; }
    run(200, true);
    iob_view_t v = view(0);
    CHECK_EQ(v.state, IOB_S_RUN);               /* a few bad frames do not drop the board */
    CHECK_EQ(v.errors, 5u);
    CHECK_EQ(v.last_error, ETH ? MBR_NO_REPLY : MBR_CRC);
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
    if (cur_link == IOB_LINK_IOPORT) iob_init_ioport(NULL, NULL); else if (ETH) iob_init_eth(NULL, NULL); else iob_init(NULL);
    iob_tags[0].enable = true;
    run(1100, true);
    CHECK_EQ(view(0).state, IOB_S_LOST);
    if (ETH) { CHECK(iob_dc_find()); run(20, true); iob_dc_view_t d; iob_dc_view(&d); CHECK_EQ(d.result, IOB_CM_NO_REPLY); }
    plat_cmd_t c = { .id = PARAM_ID_IOB_LINK, .src = PLAT_SRC_INTERNAL, .kind = PLAT_VAL_U32, .v.u = cur_link };
    CHECK_EQ(param_apply(&c), PERR_INTERLOCK);  /* no port for that link on this board: refused */
    iob_init(&bus.port);
    iob_init_eth(&net.port, &net.disc);
    iob_init_ioport(&io.port, &io.disc);
    CHECK_EQ(param_apply(&c), PERR_OK);
}
/* ---------------- Ethernet only ---------------- */
#define MASK24 0xFFFFFF00u
#define IP(x)  (0xC0A80100u + (x))              /* 192.168.1.x */

static void t_net_codec(void) {
    iob_disc_t m, d; memset(&m, 0, sizeof m);
    m.op = IOB_DISC_HERE; m.seq = 0xBEEF;
    for (unsigned k = 0; k < 6; ++k) m.uid[k] = (uint16_t)(0x1111u * (k + 1));
    iob_mac_from_uid(m.uid, m.mac);
    memcpy(m.serial, "CIO-26-00042", 12);
    m.fw_version = 0x0102; m.fw_build = 77; m.hw_rev = 2; m.flags = IOB_DISC_F_CONTROLLED;
    m.ip = IP(61); m.mask = MASK24; m.gw = IP(1); m.master_ip = SUP_IP; m.address = 1;
    uint8_t b[IOB_DISC_LEN];
    CHECK_EQ(iob_disc_encode(&m, b), IOB_DISC_LEN);
    CHECK(memcmp(b, IOB_DISC_MAGIC, 8) == 0);
    CHECK_EQ(b[54], 192u); CHECK_EQ(b[57], 61u);         /* big-endian */
    CHECK(iob_disc_decode(b, IOB_DISC_LEN, &d));
    CHECK(memcmp(&m, &d, sizeof m) == 0);
    CHECK(!iob_disc_decode(b, IOB_DISC_LEN - 1, &d));    /* wrong length */
    b[9] = 9;  CHECK(!iob_disc_decode(b, IOB_DISC_LEN, &d)); b[9] = IOB_DISC_HERE;
    b[0] = 'X'; CHECK(!iob_disc_decode(b, IOB_DISC_LEN, &d)); b[0] = 'C';
    b[30 + 3] = 0x01;                                    /* a control character ends the serial */
    CHECK(iob_disc_decode(b, IOB_DISC_LEN, &d)); CHECK(!strcmp(d.serial, "CIO"));

    /* the MAC: locally administered, from the UID */
    CHECK_EQ(m.mac[0], 0x02u);
    uint8_t mac2[6]; uint16_t uid2[6]; memcpy(uid2, m.uid, sizeof uid2); uid2[0] ^= 1u;
    iob_mac_from_uid(uid2, mac2);
    CHECK(memcmp(mac2, m.mac, 6) != 0);

    /* settings a SET may carry */
    CHECK(iob_net_settings_ok(IP(61), MASK24, IP(1), SUP_IP, 1));
    CHECK(iob_net_settings_ok(IP(61), MASK24, 0, SUP_IP, IOB_ADDR_MAX));
    CHECK(!iob_net_settings_ok(IP(61), MASK24, 0, SUP_IP, 0));
    CHECK(!iob_net_settings_ok(IP(61), MASK24, 0, SUP_IP, IOB_COMMISSION_ADDR));
    CHECK(!iob_net_settings_ok(IP(61), 0xFFFF00FFu, 0, SUP_IP, 1));       /* not contiguous */
    CHECK(!iob_net_settings_ok(IP(61), 0xFFFFFFFFu, 0, SUP_IP, 1));       /* /32 */
    CHECK(!iob_net_settings_ok(IP(0), MASK24, 0, SUP_IP, 1));             /* network address */
    CHECK(!iob_net_settings_ok(IP(255), MASK24, 0, SUP_IP, 1));           /* broadcast */
    CHECK(!iob_net_settings_ok(SUP_IP, MASK24, 0, SUP_IP, 1));            /* the supervisor's own */
    CHECK(!iob_net_settings_ok(0xC0A80261u, MASK24, 0, SUP_IP, 1));       /* another subnet */
    CHECK(!iob_net_settings_ok(IP(61), MASK24, 0xC0A80201u, SUP_IP, 1));  /* gateway outside */
    CHECK(!iob_net_settings_ok(0x7F000001u, 0xFF000000u, 0, 0x7F000002u, 1));   /* loopback */

    /* the board's rule for a SET */
    iob_disc_t set = m; set.op = IOB_DISC_SET;
    CHECK(iob_disc_set_accept(&set, m.uid, 0, IP(99)));          /* new board: any supervisor */
    CHECK(iob_disc_set_accept(&set, m.uid, SUP_IP, SUP_IP));
    CHECK(!iob_disc_set_accept(&set, m.uid, SUP_IP, IP(99)));    /* configured: only its supervisor */
    CHECK(!iob_disc_set_accept(&set, uid2, 0, SUP_IP));          /* another board's */
    set.address = 0;
    CHECK(!iob_disc_set_accept(&set, m.uid, 0, SUP_IP));
    set.address = 1; set.op = IOB_DISC_FIND;
    CHECK(!iob_disc_set_accept(&set, m.uid, 0, SUP_IP));

    /* RTU <-> Modbus TCP */
    uint8_t rtu[MBR_FRAME_MAX], tcp[IOB_MBAP_MAX], back[IOB_MBAP_MAX];
    size_t n = mbr_req_read(rtu, 1, 0, 10);
    size_t t = iob_mbap_from_rtu(rtu, n, 0x1234, tcp);
    const uint8_t want[] = { 0x12, 0x34, 0x00, 0x00, 0x00, 0x06, 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    CHECK_EQ(t, sizeof want);
    CHECK(memcmp(tcp, want, sizeof want) == 0);
    uint16_t tid = 0;
    CHECK_EQ(iob_rtu_from_mbap(tcp, t, &tid, back), n);
    CHECK(memcmp(back, rtu, n) == 0);
    CHECK_EQ(tid, 0x1234u);
    CHECK_EQ(iob_mbap_complete(tcp, 6), 0);              /* not complete yet */
    CHECK_EQ(iob_mbap_complete(tcp, t - 1), 0);
    CHECK_EQ(iob_mbap_complete(tcp, t + 3), (int)t);     /* the next frame follows */
    CHECK_EQ(iob_rtu_from_mbap(tcp, t + 3, NULL, back), 0u);
    tcp[3] = 1; CHECK_EQ(iob_mbap_complete(tcp, t), -1); tcp[3] = 0;   /* not Modbus */
    tcp[5] = 1; CHECK_EQ(iob_mbap_complete(tcp, t), -1); tcp[5] = 6;   /* too short */
    rtu[n - 1] ^= 1u;
    CHECK_EQ(iob_mbap_from_rtu(rtu, n, 1, tcp), 0u);     /* bad CRC */
    CHECK_EQ(iob_mbap_from_rtu(rtu, 3, 1, tcp), 0u);
}

static iob_dc_view_t dc_wait(void) {
    iob_dc_view_t v;
    for (int i = 0; i < 10; ++i) { run(20, true); iob_dc_view(&v); if (v.result != IOB_CM_BUSY) break; }
    return v;
}
static const iob_disc_t *dc_board(const iob_dc_view_t *v, const uint16_t uid[6]) {
    for (unsigned k = 0; k < v->n; ++k) if (!memcmp(v->boards[k].uid, uid, 12)) return &v->boards[k];
    return NULL;
}

/* commissioning from the web page: list the boards, give a new one its IP and address */
static void t_eth_commission(void) {
    setup();
    iob_node_init(&n3, 0);                      /* a new board: no IP, no address */
    memcpy(n3.serial, "CIO-26-00042", 12);
    iob_tags[0].enable = iob_tags[1].enable = iob_tags[2].enable = true;
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK_EQ(view(1).state, IOB_S_RUN);
    CHECK(view(2).state != IOB_S_RUN);

    CHECK(iob_dc_find());
    CHECK(!iob_dc_find());                      /* one request at a time */
    iob_dc_view_t v = dc_wait();
    CHECK_EQ(v.result, IOB_CM_OK);
    CHECK_EQ(v.n, 3u);
    const iob_disc_t *h = dc_board(&v, n3.uid);
    CHECK(h != NULL);
    if (!h) return;
    CHECK(h->flags & IOB_DISC_F_NEW);
    CHECK_EQ(h->ip, 0u);
    CHECK(!strcmp(h->serial, "CIO-26-00042"));
    CHECK_EQ(h->mac[0], 0x02u);
    h = dc_board(&v, n1.uid);
    CHECK(h && (h->flags & IOB_DISC_F_CONTROLLED) && h->ip == IP(61) && h->address == 1);

    /* refused before anything is sent */
    CHECK(!iob_dc_set(n3.uid, SUP_IP, MASK24, 0, SUP_IP, 3));         /* the supervisor's IP */
    CHECK(!iob_dc_set(n3.uid, IP(63), 0xFFFF00FFu, 0, SUP_IP, 3));    /* bad mask */
    CHECK(!iob_dc_set(n3.uid, IP(63), MASK24, 0, SUP_IP, 0));         /* no address */
    uint32_t d0 = net.datagrams;
    CHECK(iob_dc_set(n3.uid, IP(61), MASK24, 0, SUP_IP, 3));          /* board 1 has .61 */
    v = dc_wait();
    CHECK_EQ(v.result, IOB_CM_IN_USE);
    CHECK_EQ(net.datagrams, d0);
    CHECK_EQ(n3.ip, 0u);

    CHECK(iob_dc_set(n3.uid, IP(63), MASK24, IP(1), SUP_IP, 3));
    v = dc_wait();
    CHECK_EQ(v.result, IOB_CM_OK);
    CHECK_EQ(n3.ip, IP(63)); CHECK_EQ(n3.address, 3u); CHECK_EQ(n3.master_ip, SUP_IP); CHECK_EQ(n3.gw, IP(1));
    CHECK_EQ(v.n, 3u);                          /* the list keeps every board, with the new settings */
    h = dc_board(&v, n3.uid);
    CHECK(h && h->ip == IP(63) && !(h->flags & IOB_DISC_F_NEW));
    run(PLAT_IOB_RETRY_MS + 100, true);
    CHECK_EQ(view(2).state, IOB_S_RUN);         /* board 3 comes online by itself */
    CHECK_EQ(view(0).state, IOB_S_RUN);

    /* a board is not taken over by another supervisor */
    net.master_ip = IP(51);
    CHECK(iob_dc_set(n3.uid, IP(70), MASK24, 0, IP(51), 3));
    CHECK_EQ(dc_wait().result, IOB_CM_NO_REPLY);
    CHECK_EQ(n3.ip, IP(63));
    run(PLAT_IOB_LOST_MS + 100, true);
    CHECK_EQ(view(2).state, IOB_S_LOST);        /* and serves Modbus TCP to its own only */
    CHECK_EQ(view(0).state, IOB_S_RUN);         /* board 1 has no supervisor IP yet */
    net.master_ip = SUP_IP;
    run(PLAT_IOB_RETRY_MS + 100, true);
    CHECK_EQ(view(2).state, IOB_S_RUN);

    /* its own supervisor may move it */
    iob_tags[2].ip = IP(73);
    CHECK(iob_dc_set(n3.uid, IP(73), MASK24, 0, SUP_IP, 3));
    CHECK_EQ(dc_wait().result, IOB_CM_OK);
    run(PLAT_IOB_RETRY_MS + 100, true);
    CHECK_EQ(view(2).state, IOB_S_RUN);

    uint16_t nobody[6] = { 9, 9, 9, 9, 9, 9 };
    CHECK(iob_dc_set(nobody, IP(80), MASK24, 0, SUP_IP, 4));
    CHECK_EQ(dc_wait().result, IOB_CM_NO_REPLY);
    n1.powered = n2.powered = n3.powered = false;
    CHECK(iob_dc_find());
    v = dc_wait();
    CHECK_EQ(v.result, IOB_CM_NO_REPLY);
    CHECK_EQ(v.n, 0u);
    CHECK_EQ(v.seq, 7u);
}

/* the board's IP: a change of the setting reconnects; a board elsewhere is not answering */
static void t_eth_ip_change(void) {
    setup();
    iob_tags[0].enable = iob_tags[1].enable = true;
    memset(&cur_out, 0, sizeof cur_out);
    plat_do_set(&cur_out, 1, true);
    plat_do_set(&cur_out, 19, true);
    run(60, true);
    CHECK_EQ(n1.out_cmd, 0x0002u);
    CHECK_EQ(n2.out_cmd, 0x0002u);              /* each board gets its own outputs */
    n1.ip = IP(71);
    run(PLAT_IOB_LOST_MS + 100, true);
    CHECK_EQ(view(0).state, IOB_S_LOST);
    CHECK_EQ(view(1).state, IOB_S_RUN);
    iob_tags[0].ip = IP(71);
    run(PLAT_IOB_RETRY_MS + 100, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    /* two settings with the same IP: the second board is not run */
    iob_tags[1].ip = IP(71);
    run(40, true);
    CHECK_EQ(view(1).state, IOB_S_BAD_SETTINGS);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    iob_tags[1].ip = 0;                         /* not set */
    run(40, true);
    CHECK_EQ(view(1).state, IOB_S_BAD_SETTINGS);
    /* a unit id that is not the board's address: no answer */
    iob_tags[1].ip = IP(62); iob_tags[1].address = 9;
    run(PLAT_IOB_RETRY_MS + 100, true);
    CHECK(view(1).state != IOB_S_RUN);
    iob_tags[1].address = 2;
    run(PLAT_IOB_RETRY_MS + 100, true);
    CHECK_EQ(view(1).state, IOB_S_RUN);
}

/* changing the link (parameter 102) connects every board again over the other one */
static void t_link_change(void) {
    setup();
    iob_tags[0].enable = true;
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    uint32_t r_net = net.requests, r_bus = bus.requests;
    iob_link = IOB_LINK_RS485;
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK_EQ(net.requests, r_net);
    CHECK(bus.requests - r_bus >= 3u);
    iob_tags[0].ip = 0;                         /* the IP does not matter on RS-485 */
    run(40, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    iob_link = IOB_LINK_ETH;
    run(40, true);
    CHECK_EQ(view(0).state, IOB_S_BAD_SETTINGS);
    iob_tags[0].ip = IP(61);
    run(60, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
}

/* map 2: temperatures (settings to the board, calibration, channel states, the alarm) and the pump PWM */
static void t_temps_and_pwm(void) {
    setup();
    iob_tags[0].enable = true;
    iob_tags[0].temp_type[0] = IOB_TT_TC_K;
    iob_tags[0].temp_type[1] = IOB_TT_PT100_3W;
    iob_tags[0].temp_type[2] = IOB_TT_NTC;
    iob_tags[0].ntc_r25[2] = 270;                          /* the legacy 2.7k */
    iob_tags[0].ntc_beta[2] = 0;                           /* 0: the default B */
    iob_tags[0].pwm_hz = 400;
    iob_tags[0].first_ao = 2;
    run(100, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK_EQ(view(0).map_version, 2);
    CHECK_EQ(view(0).n_temps, IOB_TEMPS);
    CHECK_EQ(n1.temp_type[0], IOB_TT_TC_K); CHECK_EQ(n1.temp_type[1], IOB_TT_PT100_3W); CHECK_EQ(n1.temp_type[2], IOB_TT_NTC);
    CHECK_EQ(n1.ntc_r25[2], 270); CHECK_EQ(n1.ntc_beta[2], 3435);
    CHECK_EQ(n1.pwm_hz, 400); CHECK_EQ(n1.tach_ppr, 2);
    /* the board's readings, calibrated here: channel 1 +1.50 degC offset, channel 3 gain +1 % */
    n1.temp[0] = 2345; n1.temp_st[0] = IOB_TS_OK;           /* 234.5 degC */
    n1.temp[1] = IOB_TEMP_INVALID; n1.temp_st[1] = IOB_TS_OPEN;
    n1.temp[2] = 1000; n1.temp_st[2] = IOB_TS_OK;
    n1.temp[4] = -125; n1.temp_st[4] = IOB_TS_OK;
    n1.cj = 251;
    iob_tags[0].temp_offset[0] = 150;
    iob_tags[0].temp_gain[2] = 100;
    run(PLAT_IOB_TEMP_MS + 100, true);
    CHECK_EQ(last_in.rtd_cdeg[4], 23600); CHECK_EQ(last_in.rtd_status[4], PLAT_AIN_OK);   /* first_temp 5 */
    CHECK_EQ(last_in.rtd_status[5], PLAT_AIN_FAULT);
    CHECK_EQ(last_in.rtd_cdeg[6], 10100);
    CHECK_EQ(last_in.rtd_status[7], 0);                    /* an RTD has no B channel: not touched */
    CHECK_EQ(last_in.rtd_cdeg[8], -1250);
    CHECK_EQ(iob_tags[0].temps[0], 2360); CHECK_EQ(iob_tags[0].temp_state[1], IOB_TS_OPEN);
    CHECK_EQ(iob_tags[0].temp_state[3], IOB_TS_OFF);
    CHECK_EQ(iob_tags[0].cj_temp, 251);
    CHECK(view(0).status & IOBS_TEMP_FAULT);
    run(3200, true);
    CHECK(alarm_on(ALARM_ID_IOB_TEMP_FAULT));
    char lines[8][IOB_LINE];
    iob_view_t v = view(0);
    int nl = iob_fault_lines(&v, 0xFF, 0, lines, 8);
    CHECK(nl >= 1 && strcmp(lines[0], "Temp 2: open circuit") == 0);
    n1.temp_st[1] = IOB_TS_OK; n1.temp[1] = 200;
    run(PLAT_IOB_TEMP_MS + 100, true);
    CHECK(!alarm_on(ALARM_ID_IOB_TEMP_FAULT));
    /* a sensor type change goes to the board at once */
    iob_tags[0].temp_type[1] = IOB_TT_PT1000_4W;
    run(60, true);
    CHECK_EQ(n1.temp_type[1], IOB_TT_PT1000_4W);
    iob_tags[0].temp_type[1] = 9;                          /* not a type: sent as none */
    run(60, true);
    CHECK_EQ(n1.temp_type[1], IOB_TT_NONE);
    CHECK_EQ(view(0).state, IOB_S_RUN);

    /* the pump: ao_raw 2 in 0.1 %, the sign is DIR */
    cur_out.ao_raw[1] = -455;
    run(60, true);
    CHECK_EQ(n1.pwm_cmd, 455); CHECK_EQ(n1.pwm_dir, 1);
    n1.tach_rpm = 1480;
    run(60, true);
    CHECK_EQ(iob_tags[0].pump_rpm, 1480); CHECK_EQ(iob_tags[0].pump_duty, 455);
    cur_out.ao_raw[1] = 2000;                              /* clamped */
    run(60, true);
    CHECK_EQ(n1.pwm_cmd, 1000); CHECK_EQ(n1.pwm_dir, 0);
    /* stale outputs and the safe state stop the pump too */
    for (int i = 0; i < 20; ++i) step_ms(PLAT_IOB_PERIOD_MS, false);
    CHECK_EQ(n1.pwm_cmd, 0);
    run(60, true);
    CHECK_EQ(n1.pwm_cmd, 1000);
    iob_safe_state();
    run(60, true);
    CHECK_EQ(n1.pwm_cmd, 0);
    /* the board stops answering: the configured channels are faults for the application */
    n1.powered = false;
    run(1500, true);
    CHECK_EQ(view(0).state, IOB_S_LOST);
    CHECK_EQ(last_in.rtd_status[4], PLAT_AIN_FAULT);
    CHECK_EQ(iob_tags[0].temps[0], IOB_TEMP_INVALID);
    CHECK_EQ(iob_tags[0].temp_state[0], IOB_TS_WAIT);
    cur_out.ao_raw[1] = 0;
}

/* a map 1 board (G0 firmware) still runs: no PWM written, no temperatures read */
static void t_map1_board(void) {
    setup();
    n1.map_version = 1; n1.n_temp_pwm = 0;
    iob_tags[0].enable = true;
    iob_tags[0].temp_type[0] = IOB_TT_TC_J;
    iob_tags[0].first_ao = 1;
    cur_out.ao_raw[0] = 500;
    cur_out.do_bits = 0x3;
    run(200, true);
    CHECK_EQ(view(0).state, IOB_S_RUN);
    CHECK_EQ(view(0).map_version, 1);
    CHECK_EQ(view(0).n_temps, 0);
    CHECK_EQ(n1.out_cmd, 0x3);
    CHECK_EQ(view(0).errors, 0u);                          /* nothing it would refuse was sent */
    CHECK_EQ(iob_tags[0].temp_state[0], IOB_TS_ADC);       /* configured, but the board has no inputs */
    CHECK_EQ(last_in.rtd_status[4], PLAT_AIN_FAULT);
    n1.map_version = 3;                                    /* a later map: not this master's board */
    iob_node_restart(&n1, false, bsp_host.ms);
    n1.powered = false; run(1200, true); n1.powered = true;
    run(1200, true);
    CHECK_EQ(view(0).state, IOB_S_WRONG_TYPE);
    cur_out.do_bits = 0; cur_out.ao_raw[0] = 0;
}

void test_iobus_all(void) {
    t_node_cfg();
    t_comms_latch();
    t_input_pulse();
    RUN(t_rtu_frames);
    RUN(t_node_rules);
    RUN(t_net_codec);
    for (cur_link = IOB_LINK_RS485; cur_link <= IOB_LINK_IOPORT; ++cur_link) {
        RUN(t_off_by_default);
        RUN(t_connect_and_io);
        RUN(t_channel_ranges);
        RUN(t_stale_and_safe);
        RUN(t_lost_and_back);
        RUN(t_node_restart);
        RUN(t_faults);
        RUN(t_wrong_type_and_conflicts);
        RUN(t_commission);
        RUN(t_lost_replies);
        RUN(t_node_timeout);
        RUN(t_no_port);
        RUN(t_temps_and_pwm);
        RUN(t_map1_board);
    }
    cur_link = IOB_LINK_ETH;
    RUN(t_eth_commission);
    RUN(t_eth_ip_change);
    RUN(t_link_change);
    iob_reset();
    memset(iob_tags, 0, sizeof iob_tags);
    param_set_defaults(PARAM_ALL);
}
