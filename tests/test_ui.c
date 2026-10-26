/* test_ui.c - the EVE driver against the simulated BT817, and the touchscreen UI
 * driven by simulated touches: navigation, login, editing every kind of entry,
 * mode changes with confirmation, events, firmware activation, idle behaviour.
 * With UI_DUMP=<dir> set, each screen's command list is written there for
 * tools/eve_preview.py. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "tst.h"
#include "eve.h"
#include "eve_sim.h"
#include "ui.h"
#include "param.h"
#include "param_store.h"
#include "plat_log.h"
#include "fw_update.h"
#include "fw_flash_sim.h"
#include "web_auth.h"
#include "plat_sha256.h"
#include "bsp_host.h"
#include "ul_system.h"
#include "log_csv.h"
#include "plat_alarm.h"
#include "plat_clock.h"
#include "iobus.h"
#include "iob_node_sim.h"
#include "plat_state.h"
#include "plat_power.h"
#include "plat_cal.h"
#include "plat_service.h"

static eve_sim_t sim;
static eve_t eve;
static plat_cmdq_t q;
static plat_seqlock_t lk;

/* an application table like the demo's */
static uint32_t a_delay = 2000, a_run = 5;
static bool a_valve, a_in;
static float a_gain = 1.5f, a_temp = 182.5f, a_zero = 0.0f;
static char a_name[11] = "Valve 1";
static const char name_def[11] = "Valve 1";       /* defaults are the full size of the block */
static bool a_ack[4];
static const param_def_t defs[] = {
    PARAM_UINT(1001, "start_delay", a_delay, CT_U32, 100, 60000, 2000, PLAT_LVL_OPERATOR, PF_PERSIST, "ms"),
    PARAM_UINT(1002, "run_seconds", a_run, CT_U32, 0, 0xFFFFFFFFu, 0, PLAT_LVL_VIEW, PF_RO, "s"),
    PARAM_BIT (1003, "valve_out", a_valve, PLAT_LVL_VIEW, PF_RO),
    PARAM_BIT (1004, "start_in", a_in, PLAT_LVL_VIEW, PF_RO),
    PARAM_TEXT(1005, "valve_name", a_name, name_def, PLAT_LVL_OPERATOR, PF_PERSIST),
    PARAM_ARRAY_BOOL(1006, "ack", a_ack, PLAT_LVL_OPERATOR, PF_ANY_MODE),
    PARAM_REAL(1007, "gain", a_gain, 0.0f, 10.0f, 1.5f, NULL, PLAT_LVL_ADMIN, PF_PERSIST | PF_CONFIRM),
    PARAM_REAL(1008, "stack_temp", a_temp, -50.0f, 500.0f, 182.5f, "\xC2\xB0" "C", PLAT_LVL_VIEW, PF_RO),
    PARAM_REAL(1009, "zero_offset", a_zero, -100.0f, 100.0f, 0.0f, NULL, PLAT_LVL_FACTORY, PF_PERSIST | PF_FACTORY),
};
static const uint16_t home_ids[] = { 1001, 1002, 1003, 1005 };
static const ui_cfg_t cfg = { .product = "CEMS Supervisor", .version = "0.5.0", .home_ids = home_ids, .n_home = 4 };

static uint32_t now = 100000;
static void scan(void) {
    plat_cmd_t c; while (plat_cmdq_pop(&q, &c)) (void)param_apply(&c);
    param_blob_drain(); plat_clock_scan(); alarm_scan(now); param_publish();
}
static void frame(void) { now += 20; plat_log_poll(now); ui_draw(&eve, now); }
static void press(uint8_t tag) { ui_input(tag, now); now += 60; ui_input(0, now); scan(); plat_log_poll(now); frame(); }
static bool tap(const char *label) {
    int t = eve_sim_tag_of(&sim, label);
    if (t <= 0) { printf("  (no button \"%s\")\n", label); return false; }
    press((uint8_t)t);
    return true;
}
static bool shows(const char *s) { return eve_sim_has_text(&sim, s); }
/* page the settings list until the row shows, then tap it */
static bool on_page(const char *name) { return eve_sim_tag_of(&sim, name) > 0; }   /* the row itself, not a longer name */
static bool row(const char *name) {
    for (int k = 0; k < 200 && eve_sim_tag_of(&sim, "Up") > 0 && !on_page(name); ++k) tap("Up");
    if (!on_page(name)) for (int k = 0; k < 200 && !on_page(name); ++k) if (!tap("Down")) break;
    return tap(name);
}
static void type(const char *s) { for (; *s; ++s) press((uint8_t)*s); }
static void dump(const char *name) {
    const char *dir = getenv("UI_DUMP");
    if (!dir) return;
    char p[256]; snprintf(p, sizeof p, "%s/%s.eve", dir, name);
    FILE *f = fopen(p, "wb"); if (f) { eve_sim_dump(&sim, f); fclose(f); }
}

static void t_fonts(void) {
    eve_sim_init(&sim);
    CHECK_EQ(eve_init(&eve, &sim.bus, &eve_panel_rvt70h), 0);
    CHECK_EQ(eve_fonts_load(&eve, EVE_FONT_BASE), 0x0F);                     /* all four inflated and CRC-checked */
    CHECK_EQ(sim.inflates, 4); CHECK_EQ(sim.memcrcs, 4); CHECK_EQ(sim.inflate_errors, 0);
    /* glyph data where the metric block says, minus one cell for code 0 (first character is 1) */
    uint32_t m0 = eve.font_metric[0];
    uint32_t ptr = (uint32_t)sim.ram_g[m0 + 144] | (uint32_t)sim.ram_g[m0 + 145] << 8 | (uint32_t)sim.ram_g[m0 + 146] << 16 | (uint32_t)sim.ram_g[m0 + 147] << 24;
    CHECK_EQ(ptr + eve_fonts[0].cell, (m0 + 148u + 3u) & ~3u);
    CHECK_EQ(sim.ram_g[m0 + 128], 2);                             /* L4 */
    CHECK_EQ(sim.ram_g[m0 + 'A'] > 0, 1);                         /* a width for 'A' */
    /* the CRC check reads the chip's answer back from the FIFO */
    uint32_t crc = 0;
    CHECK_EQ(eve_memcrc(&eve, eve.font_metric[0], 148, &crc), 0);
    { uint8_t m[148]; memcpy(m, sim.ram_g + eve.font_metric[0], 148);
      uint32_t c = 0xFFFFFFFFu; for (int i = 0; i < 148; ++i) { c ^= m[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
      CHECK_EQ(crc, ~c); }
    /* a co-processor fault while loading: that font is not used (ROM font instead), the rest are */
    eve_sim_init(&sim);
    CHECK_EQ(eve_init(&eve, &sim.bus, &eve_panel_rvt70h), 0);
    sim.fault_next = true;
    uint8_t ok = eve_fonts_load(&eve, EVE_FONT_BASE);
    CHECK_EQ(ok & 1u, 0); CHECK_EQ(ok, 0x0E);
    CHECK_EQ(eve.faults, 1);
    /* a damaged stream: the chip reports nothing useful, the CRC does not match */
    static uint8_t bad[8192];
    uint32_t zl = eve_fonts[1].zlen < sizeof bad ? eve_fonts[1].zlen : (uint32_t)sizeof bad;
    memcpy(bad, eve_fonts[1].z, zl);
    bad[zl / 2] ^= 0x55;
    (void)eve_inflate(&eve, 0x80000u, bad, zl);
    CHECK_EQ(sim.inflate_errors, 1);
    /* encoding: symbols to codes 1..31 with our fonts, '?' with the ROM fonts */
    char b[32];
    CHECK(!strcmp(eve_font_encode("12 \xC2\xB0" "C", b, sizeof b, true), "12 \x01" "C"));
    CHECK(!strcmp(eve_font_encode("mg/m\xC2\xB3", b, sizeof b, true), "mg/m\x03"));
    CHECK(!strcmp(eve_font_encode("\xCE\xBCg", b, sizeof b, true), "\x04g"));        /* Greek mu -> micro */
    CHECK(!strcmp(eve_font_encode("mg/m\xC2\xB3", b, sizeof b, false), "mg/m?"));
    CHECK(!strcmp(eve_font_encode("\xE4\xB8\xAD!", b, sizeof b, true), "?!"));       /* not in the font */
    CHECK(!strcmp(eve_font_encode("a\xC2", b, sizeof b, true), "a?"));                 /* cut short */
    CHECK(!strcmp(eve_font_encode("tab\there", b, sizeof b, true), "tabhere"));
}

static void t_eve_driver(void) {
    eve_sim_init(&sim);
    CHECK_EQ(eve_init(&eve, &sim.bus, &eve_panel_rvt70h), 0);
    CHECK(eve.ok);
    CHECK_EQ(sim.host_cmds, 2);                                  /* CLKEXT, ACTIVE */
    CHECK_EQ(sim.regs[REG_HSIZE - EVE_RAM_REG] | (sim.regs[REG_HSIZE - EVE_RAM_REG + 1] << 8), 1024);
    CHECK_EQ(sim.regs[REG_VCYCLE - EVE_RAM_REG] | (sim.regs[REG_VCYCLE - EVE_RAM_REG + 1] << 8), 635);
    CHECK_EQ(sim.regs[REG_PCLK - EVE_RAM_REG], 1);
    CHECK_EQ(sim.frames, 1);
    /* a frame with text, a button and a number */
    eve_frame_begin(&eve, 0, 0, 0);
    eve_tag(&eve, 7);
    eve_button(&eve, 10, 10, 100, 60, 28, 0, "Press");
    eve_tag(&eve, 0);
    eve_text(&eve, 10, 100, 28, 0, "Hello EVE");
    eve_number(&eve, 10, 200, 28, 0, -42);
    CHECK_EQ(eve_frame_end(&eve), 0);
    CHECK_EQ(sim.frames, 2);
    CHECK(eve_sim_has_text(&sim, "Hello EVE"));
    CHECK_EQ(eve_sim_tag_of(&sim, "Press"), 7);
    /* touch */
    eve_sim_touch(&sim, 7); CHECK_EQ(eve_touch_tag(&eve), 7);
    eve_sim_touch(&sim, 0); CHECK_EQ(eve_touch_tag(&eve), 0);
    /* co-processor fault: message kept, co-processor reset, next frame fine */
    sim.fault_next = true;
    eve_frame_begin(&eve, 0, 0, 0);
    CHECK_EQ(eve_frame_end(&eve), -1);
    CHECK_EQ(eve.faults, 1);
    CHECK(strstr(eve.fault_msg, "test fault") != NULL);
    eve_frame_begin(&eve, 0, 0, 0);
    CHECK_EQ(eve_frame_end(&eve), 0);
    /* overflow is caught, never written past the buffer */
    eve_frame_begin(&eve, 0, 0, 0);
    for (int k = 0; k < 2000; ++k) eve_text(&eve, 0, 0, 28, 0, "overflowing the command buffer");
    CHECK(eve.overflow);
    CHECK_EQ(eve_frame_end(&eve), 0);
    /* no display connected */
    eve_sim_t none; eve_sim_init(&none); none.present = false;
    eve_t e2;
    CHECK_EQ(eve_init(&e2, &none.bus, &eve_panel_rvt70h), -1);
    CHECK(!e2.ok);
    CHECK_EQ(eve_touch_tag(&e2), 0);
}

static uint8_t fram[PLAT_FRAM_SIZE];
static uint8_t nor_mem[64 * 1024];
static uint8_t bank0[64 * 1024], bank1[64 * 1024];
static fw_flash_t fl; static fw_flash_sim_t fs;

static void setup(void) {
    static nvm_dev_t d; static nvm_ram_t r;
    static nor_dev_t nor; static nor_sim_t ns;
    memset(fram, 0, sizeof fram);
    nvm_ram_init(&d, &r, fram, sizeof fram, true);
    CHECK_EQ(param_init(defs, sizeof defs / sizeof defs[0]), PERR_OK);
    CHECK(param_store_init(&d, NULL, NULL, 0, 1, NULL));
    CHECK_EQ(alarm_init(NULL, 0, BSP_RESET_POWER_ON), 0);
    param_store_boot();
    plat_cmdq_init(&q); lk.seq = 0;
    param_bind(&q, &lk);
    auth_init();
    plat_log_reset_for_test();
    nor_sim_init(&nor, &ns, nor_mem, sizeof nor_mem, true, true);
    plat_log_cfg_t lc = { .nor = &nor, .fram = &d, .fram_addr = STORE_META_ADDR, .fw_version = "0.5.0", .unix_now = 1792411200u };
    CHECK(plat_log_init(&lc, now));
    fw_reset_for_test();
    fw_flash_sim_init(&fl, &fs, bank0, bank1, sizeof bank0);
    fw_init(&fl, &d, STORE_META_ADDR + 128u, "0.5.0");
    RTC_Date.Year = 2026; RTC_Date.Month = 10; RTC_Date.Day_In_Month = 20;
    RTC_Time.Hours = 9; RTC_Time.Minutes = 41; RTC_Time.Seconds = 7;
    param_publish();
    eve_sim_init(&sim);
    CHECK_EQ(eve_init(&eve, &sim.bus, &eve_panel_rvt70h), 0);
    CHECK_EQ(eve_fonts_load(&eve, EVE_FONT_BASE), 0x0F);                     /* as the UI task does */
    ui_init(&cfg, now);
    frame();
}

static void stage_firmware(void) {          /* an image in the spare bank, as the web upload leaves it */
    static uint8_t img[FW_HDR_SIZE + 4096];
    fw_header_t h; memset(&h, 0, sizeof h);
    h.magic = FW_MAGIC; h.hdr_size = FW_HDR_SIZE; h.hdr_version = 1; h.product = PLAT_FW_PRODUCT;
    strcpy(h.version_str, "0.6.0"); h.version = fw_parse_version("0.6.0"); h.image_size = 4096;
    for (int k = 0; k < 4096; ++k) img[FW_HDR_SIZE + k] = (uint8_t)(k * 7);
    sha256_t s; sha256_init(&s); sha256_update(&s, img + FW_HDR_SIZE, 4096); sha256_final(&s, h.sha256);
    h.hdr_crc = plat_crc32(0, &h, (uint32_t)offsetof(fw_header_t, hdr_crc));
    memcpy(img, &h, sizeof h);
    CHECK_EQ(fw_upload_begin(sizeof img), FW_OK);
    CHECK_EQ(fw_upload_data(img, sizeof img), FW_OK);
    CHECK_EQ(fw_upload_end(), FW_OK);
}

static void t_ui(void) {
    setup();
    scan(); frame();
    /* home */
    CHECK_EQ(ui_screen(), UI_HOME);
    CHECK(shows("CEMS Supervisor")); CHECK(shows("LOCAL")); CHECK(shows("1 ALARM  1 NEW")); CHECK(shows("09:41:07"));
    CHECK(shows("start_delay")); CHECK(shows("2000 ms")); CHECK(shows("Valve 1"));
    CHECK(shows("Control: LOCAL"));
    dump("home");

    /* settings: everything readable; changes need a login */
    CHECK(tap("Settings"));
    CHECK_EQ(ui_screen(), UI_PARAMS);
    CHECK(shows("ctrl_mode")); CHECK(shows("comms_timeout")); CHECK(shows("LOCAL"));
    dump("settings");
    CHECK(row("start_delay"));
    CHECK_EQ(ui_screen(), UI_PARAMS);
    CHECK(strstr(ui_toast(), "Log in as operator") != NULL);

    /* a unit the ROM fonts cannot draw: shown with our font (degree sign = code 1) */
    for (int k = 0; k < 10 && !shows("stack_temp"); ++k) if (!tap("Down")) break;
    CHECK(shows("182.5 \x01" "C"));
    dump("settings_units");
    for (int k = 0; k < 10 && eve_sim_tag_of(&sim, "Up") > 0; ++k) tap("Up");
    /* log in: wrong, then right */
    CHECK(tap("Log in"));
    CHECK_EQ(ui_screen(), UI_LOGIN);
    dump("login");
    type("1112"); CHECK(tap("Enter"));
    CHECK_EQ(ui_level(), 0); CHECK(!strcmp(ui_toast(), "Wrong password"));
    type("1111"); CHECK(tap("Enter"));
    CHECK_EQ(ui_level(), PLAT_LVL_OPERATOR);
    CHECK_EQ(ui_screen(), UI_PARAMS);
    CHECK(shows("Log out operator"));

    /* numeric entry: refused out of range, then accepted */
    CHECK(row("start_delay"));
    CHECK_EQ(ui_screen(), UI_EDIT);
    CHECK(shows("Range 100 .. 60000 ms")); CHECK(shows("Now: 2000 ms"));
    dump("edit_number");
    type("99"); CHECK(tap("OK"));
    CHECK(!strcmp(ui_toast(), "Outside the allowed range")); CHECK_EQ(ui_screen(), UI_EDIT);
    CHECK(tap("Clear")); type("2500"); CHECK(tap("OK"));
    CHECK_EQ(ui_screen(), UI_PARAMS); CHECK(!strcmp(ui_toast(), "Saved"));
    CHECK_EQ(a_delay, 2500u);
    /* text entry with capitals */
    CHECK(row("valve_name"));
    CHECK(tap("Clear"));
    CHECK(tap("ABC")); type("I"); CHECK(tap("abc")); type("nlet"); CHECK(tap("Space")); type("3");
    CHECK(shows("Inlet 3_"));
    dump("edit_text");
    CHECK(tap("OK"));
    CHECK(!strcmp(a_name, "Inlet 3"));
    /* arrays are for the web page */
    CHECK(row("ack"));
    CHECK(strstr(ui_toast(), "web page") != NULL);

    /* hand control to REMOTE (confirmed), then the touchscreen can't change setpoints */
    CHECK(tap("Home"));
    CHECK(tap("Hand to REMOTE"));
    CHECK(shows("Hand control to the PLC and web (REMOTE)?"));
    dump("confirm");
    CHECK(tap("No"));
    CHECK_EQ(param_ctrl_mode(), PARAM_MODE_LOCAL);
    CHECK(tap("Hand to REMOTE")); CHECK(tap("Yes"));
    CHECK_EQ(param_ctrl_mode(), PARAM_MODE_REMOTE);
    CHECK(shows("REMOTE")); CHECK(shows("Take LOCAL control"));
    CHECK(tap("Settings"));
    CHECK(row("start_delay"));
    CHECK(strstr(ui_toast(), "REMOTE mode") != NULL);
    /* the local operator can always take control back, even logged out */
    CHECK(tap("Log out operator"));
    CHECK_EQ(ui_level(), 0);
    CHECK(tap("Home")); CHECK(tap("Take LOCAL control")); CHECK(tap("Yes"));
    CHECK_EQ(param_ctrl_mode(), PARAM_MODE_LOCAL);
    /* ...but needs a login to hand it over */
    CHECK(tap("Hand to REMOTE"));
    CHECK(strstr(ui_toast(), "Log in as operator") != NULL);

    /* admin: IP address entry with the network confirmation; on/off; PF_CONFIRM */
    CHECK(tap("Log in")); CHECK(tap("Admin")); type("2222"); CHECK(tap("Enter"));
    CHECK_EQ(ui_level(), PLAT_LVL_ADMIN);
    CHECK(tap("Settings"));
    CHECK(row("net_ip"));
    CHECK(shows("192.168.1.50_"));
    dump("edit_ip");
    CHECK(tap("Clear")); type("10.0.0.5"); CHECK(tap("OK"));
    CHECK(shows("Change the network settings now?"));
    CHECK(tap("Yes"));
    plat_val_t v; CHECK(param_read(PARAM_ID_NET_IP, &v)); CHECK_EQ(v.u, 0x0A000005u);
    CHECK(row("net_mask")); CHECK(tap("Clear")); type("255.255.0"); CHECK(tap("OK"));
    CHECK(strstr(ui_toast(), "Enter an address") != NULL); CHECK(tap("Cancel"));
    CHECK(row("modbus_write_en"));
    dump("edit_bool");
    CHECK(tap("OFF"));
    CHECK(param_read(PARAM_ID_MODBUS_WRITE_EN, &v)); CHECK(!v.b);
    CHECK(row("gain")); type("2.75"); CHECK(tap("OK"));
    CHECK(shows("Change gain?")); CHECK(tap("Yes"));
    CHECK(fabsf(a_gain - 2.75f) < 1e-6f);

    /* events: the audit trail of all that, with the touchscreen as the source */
    CHECK(tap("Events"));
    for (int k = 0; k < 5; ++k) frame();
    CHECK(shows("Setting changed")); CHECK(shows("gain (1007): 1.5 -> 2.75"));
    CHECK(shows("touchscreen / admin"));
    dump("events");
    CHECK(tap("Older")); for (int k = 0; k < 5; ++k) frame();
    CHECK(shows("Login failed") || shows("Mode"));

    /* system: firmware staged; admin can't activate it, tech (factory) level can, after confirming */
    stage_firmware();
    CHECK(tap("System"));
    CHECK(shows("Running")); CHECK(shows("0.5.0")); CHECK(shows("Restart on 0.6.0")); CHECK(shows("192.168.1.50") || shows("10.0.0.5"));
    CHECK(shows("not measured"));                     /* supply: no reading on this board */
    plat_power_reset_for_test();
    bsp_host.supply_mv = 24130;
    for (int k = 0; k < 32; ++k) plat_power_sample();
    frame();
    CHECK(shows("24.1 V"));
    { char crc[12]; snprintf(crc, sizeof crc, "%08lX", (unsigned long)plat_config_crc());
      CHECK(shows(crc)); CHECK(shows("since service")); CHECK(shows("build ")); }
    CHECK(shows("Self-test"));
    CHECK(shows("running") || shows("passed (") || shows("failed: "));   /* CIP-11.1 */   /* CIP-05.4, operating hours, build ID */
    CHECK(shows("Clock, not synced"));             /* clock quality (CIP-02.6): never set from outside in this test */
    dump("system");
    bsp_host.supply_mv = 0; plat_power_reset_for_test();
    uint32_t swaps = fs.swaps;
    /* a restart asked from the web, held for the touchscreen (parameter 87): the question pops up, "No" refuses it */
    { plat_cmd_t c = { .id = PARAM_ID_FW_LOCAL_CONFIRM, .src = PLAT_SRC_INTERNAL, .kind = PLAT_VAL_BOOL, .v.b = true };
      CHECK_EQ(param_apply(&c), PERR_OK); param_publish(); }
    CHECK_EQ(fw_request_activate_from(PLAT_SRC_WEB, PLAT_LVL_FACTORY), FW_E_WAIT_LOCAL);
    frame();
    CHECK(shows("Restart on firmware 0.6.0? Asked from the"));
    dump("fw_confirm");
    CHECK(tap("No"));
    CHECK(!strcmp(ui_toast(), "Firmware restart refused"));
    { fw_status_t fst; fw_status(&fst); CHECK(!fst.wait_local); }
    fw_service(now, true);
    CHECK_EQ(fs.swaps, swaps);
    { plat_cmd_t c = { .id = PARAM_ID_FW_LOCAL_CONFIRM, .src = PLAT_SRC_INTERNAL, .kind = PLAT_VAL_BOOL, .v.b = false };
      CHECK_EQ(param_apply(&c), PERR_OK); param_publish(); }
    CHECK_EQ(eve_sim_tag_of(&sim, "Restart on 0.6.0"), 0);          /* disabled for admin */
    CHECK(tap("Log out admin"));
    CHECK(tap("Log in")); CHECK(tap("Factory")); type("9999"); CHECK(tap("Enter"));
    CHECK_EQ(ui_level(), PLAT_LVL_FACTORY);
    CHECK(tap("Restart on 0.6.0")); CHECK(shows("Restart on firmware 0.6.0 now?")); CHECK(tap("Yes"));
    fw_service(now, true);
    CHECK_EQ(fs.swaps, swaps + 1);
    CHECK(shows("80 %"));
    /* set the clock from the System screen (factory login: admin or above) */
    CHECK(tap("Set clock"));
    CHECK_EQ(ui_screen(), UI_CLOCK);
    CHECK(tap("Clear")); type("2026110515"); CHECK(tap("OK"));
    CHECK(strstr(ui_toast(), "Enter year") != NULL);                         /* minutes missing */
    type("30"); CHECK(shows("2026-11-05 15:30:__"));
    dump("clock");
    CHECK(tap("OK"));
    CHECK(!strcmp(ui_toast(), "Clock set")); CHECK_EQ(ui_screen(), UI_SYSTEM);
    CHECK_EQ(bsp_host.rtc.year, 2026); CHECK_EQ(bsp_host.rtc.month, 11); CHECK_EQ(bsp_host.rtc.day, 5);
    CHECK_EQ(bsp_host.rtc.hours, 15); CHECK_EQ(bsp_host.rtc.minutes, 30); CHECK_EQ(bsp_host.rtc.dow_iso, 4);
    CHECK(tap("-")); CHECK_EQ(ui_brightness(now), 60);

    /* lockout after five wrong passwords */
    CHECK(tap("Log out factory"));
    CHECK(tap("Log in"));
    for (int k = 0; k < 5; ++k) { type("0000"); CHECK(tap("Enter")); }
    CHECK(strstr(ui_toast(), "Locked") != NULL);
    CHECK_EQ(eve_sim_tag_of(&sim, "Enter"), 0);              /* disabled while locked */
    CHECK(shows("Too many attempts"));
    now += 31000; frame();
    type("1111"); CHECK(tap("Enter"));
    CHECK_EQ(ui_level(), PLAT_LVL_OPERATOR);

    /* slide off a button: nothing happens */
    int home = eve_sim_tag_of(&sim, "Home"), sys = eve_sim_tag_of(&sim, "System");
    ui_input((uint8_t)home, now); ui_input((uint8_t)sys, now + 20); ui_input(0, now + 40); frame();
    CHECK(ui_screen() != UI_HOME);

    /* idle: the login lapses, the screen dims, the first touch only wakes it */
    now += 11u * 60u * 1000u;
    ui_input(0, now); frame();
    CHECK_EQ(ui_level(), 0);
    CHECK_EQ(ui_brightness(now), 10);
    int scr = ui_screen();
    press((uint8_t)eve_sim_tag_of(&sim, "Home"));
    CHECK_EQ(ui_screen(), scr);
    CHECK(ui_brightness(now) > 10);
    CHECK(tap("Home")); CHECK_EQ(ui_screen(), UI_HOME);

    /* duplicate address: alarm in the header, explained on the System screen and in the log text */
    plat_net_status_word = NETST_CONFLICT | NETST_LINK;
    now += 300; scan(); frame();
    CHECK(shows("2 ALARMS  2 NEW"));
    CHECK(tap("System")); CHECK(shows("IN USE by another device"));
    dump("ip_conflict");
    /* the alarm list, from the header pill: acknowledge one, then all (no login needed) */
    CHECK(tap("2 ALARMS  2 NEW"));
    CHECK_EQ(ui_screen(), UI_ALARMS);
    CHECK(shows("No valid calibration data: measurement blocked"));
    CHECK(shows("IP address in use by another device: network off"));
    CHECK(shows("Ack all"));
    dump("alarms");
    /* tap an alarm: a panel shows "what to check" (alarm_def_t.help); tap again to go back */
    CHECK(!shows("What to check"));
    CHECK(tap("No valid calibration data: measurement blocked"));
    CHECK(shows("What to check"));
    dump("alarm_help");
    CHECK(tap("No valid calibration data: measurement blocked"));
    CHECK(!shows("What to check"));
    CHECK(tap("Acknowledge"));
    CHECK(!strcmp(ui_toast(), "Acknowledged"));
    CHECK(shows("2 ALARMS  1 NEW"));
    CHECK(tap("Ack all"));
    CHECK(shows("2 ALARMS"));
    CHECK(!shows("Acknowledge"));
    /* alarm actions: shown to everyone, changed by admin only */
    CHECK(tap("Actions"));
    CHECK_EQ(ui_screen(), UI_ALARM_CFG);
    CHECK(shows("CAL_INVALID")); CHECK(shows("Email")); CHECK(shows("R4"));
    CHECK_EQ(eve_sim_tag_of(&sim, "R1"), 0);                          /* not logged in: not touchable */
    CHECK(tap("Log in")); CHECK(tap("Admin")); type("2222"); CHECK(tap("Enter"));
    CHECK_EQ(ui_screen(), UI_ALARM_CFG);
    CHECK(alarm_route(ALARM_ID_CAL_INVALID) == (AR_EMAIL | AR_MODBUS));
    CHECK(tap("R1"));                                                  /* first row: CAL_INVALID, relay 1 on */
    CHECK_EQ(alarm_route(ALARM_ID_CAL_INVALID), AR_EMAIL | AR_MODBUS | AR_RELAY(0));
    CHECK(tap("Email"));                                               /* and email off */
    CHECK_EQ(alarm_route(ALARM_ID_CAL_INVALID), AR_MODBUS | AR_RELAY(0));
    dump("alarm_actions");
    CHECK(tap("Back")); CHECK_EQ(ui_screen(), UI_ALARMS);
    CHECK(tap("Log out admin"));
    { log_event_t ev = { .code = LOG_EV_IP_CONFLICT, .arg = 0xC0A80132u, .new_v = 1.0, .old_v = NAN };
      char w[32], dt[64]; log_event_text(&ev, w, sizeof w, dt, sizeof dt);
      CHECK(!strcmp(w, "Address conflict")); CHECK(!strcmp(dt, "192.168.1.50 is used by another device")); }
    plat_net_status_word = 0;
    now += 300; scan(); frame();
    CHECK(shows("1 ALARM"));
    CHECK(!shows("IP address in use by another device: network off"));      /* acknowledged and gone */
    for (int k = 0; k < 5; ++k) frame();
    CHECK(tap("Events")); for (int k = 0; k < 5; ++k) frame();
    CHECK(shows("Alarm cleared") || shows("Alarm acknowledged"));
    CHECK_EQ(eve.overflow, false);
    CHECK_EQ(eve.faults, 0);
    plat_log_reset_for_test();
    param_on_change = NULL;
    fw_reset_for_test();
}

/* CIP-21.2, 21.4, 21.5: the state and link pills in the header; a value's quality on the home
 * tile and the settings row (UI_CONVENTIONS 3, 4) */
static const plat_meas_def_t ui_meas[] = {
    { .id = 1002, .index = PLAT_MEAS_NO_INDEX, .max_age_ms = 1000, .lo = 0.0f, .hi = 1000.0f },
};
static void st_scan(void) { plat_state_scan(bsp_ms()); param_publish(); frame(); }   /* measurements are stamped by bsp_ms() */
static void t_state_quality(void) {
    setup();
    plat_state_reset_for_test();
    CHECK_EQ(plat_state_init(ui_meas, 1), 0);
    a_run = 12;
    st_scan();
    CHECK(shows("READY"));                            /* not measuring: readings unavailable */
    CHECK(shows("NO LINK"));
    CHECK(shows("----")); CHECK(shows("unavailable")); CHECK(!shows("12 s"));
    plat_app_state(PLAT_ST_MEASURING);
    st_scan();
    CHECK(shows("MEASURING"));
    CHECK(shows("unavailable"));                      /* measuring, but never updated */
    plat_meas_update(0, PLAT_Q_VALID);
    st_scan();
    CHECK(shows("12 s")); CHECK(!shows("unavailable")); CHECK(!shows("stale"));
    dump("home_measuring");
    bsp_host_advance_ms(1500);                        /* not updated within max_age_ms */
    st_scan();
    CHECK(shows("12 s")); CHECK(shows("stale"));
    a_run = 5000; plat_meas_update(0, PLAT_Q_VALID);
    st_scan();
    CHECK(shows("out of range"));
    CHECK(tap("Settings"));
    CHECK(row("run_seconds"));                        /* the settings row carries it too */
    CHECK(tap("Home"));
    CHECK(shows("out of range"));
    /* the supervisory link: a web or Modbus request, then quiet for longer than comms_timeout (10 s) */
    param_comms_activity(PLAT_SRC_WEB, now);
    st_scan();
    CHECK(shows("LINK OK"));
    now += 10500;
    st_scan();
    CHECK(shows("LINK LOST"));
    dump("home_quality");
    /* MAINTENANCE from the home screen: operator, confirmed, readings marked */
    CHECK(tap("Maintenance"));
    CHECK(shows("Log in as operator to enter maintenance"));
    CHECK(tap("Log in")); CHECK(tap("Operator")); type("1111"); CHECK(tap("Enter"));
    CHECK(ui_level() >= PLAT_LVL_OPERATOR);
    CHECK(tap("Home"));
    CHECK(tap("Maintenance"));
    CHECK(shows("Start maintenance?"));
    CHECK(tap("Yes"));
    scan(); st_scan();
    CHECK(shows("Maintenance started"));
    CHECK(shows("End maintenance"));
    CHECK(plat_state() == PLAT_ST_MAINTENANCE || plat_state() == PLAT_ST_FAULT);
    CHECK(tap("End maintenance"));
    CHECK(tap("Yes"));
    scan(); st_scan();
    CHECK(shows("Maintenance ended"));
    CHECK(shows("Maintenance"));
    plat_state_reset_for_test();
}

/* CIP-18: System -> Calibrate: a refusal in words, start, the procedure's points, accept, the
 * record in force and the history, revert to factory */
static const plat_cal_channel_t ui_cal[] = { { .name = "Zero gas", .coef_id = { 1009 }, .interval_days = 90 } };
static void cal_scan(void) {
    for (int k = 0; k < 3; ++k) { scan(); bsp_host_advance_ms(10); plat_cal_scan(bsp_ms()); plat_state_scan(bsp_ms()); }
    param_publish(); frame();
}
static void t_cal_screen(void) {
    setup();
    plat_state_reset_for_test(); plat_cal_reset_for_test();
    CHECK_EQ(plat_cal_init(ui_cal, 1), 0);
    static nvm_dev_t d; static nvm_ram_t r;
    memset(fram, 0, sizeof fram);
    nvm_ram_init(&d, &r, fram, sizeof fram, true);
    CHECK(param_store_init(&d, NULL, NULL, 0, 1, NULL));
    param_store_boot();
    cal_scan();
    CHECK(tap("System"));
    CHECK(tap("Calibrate"));
    CHECK_EQ(ui_screen(), UI_CAL);
    CHECK(shows("Zero gas")); CHECK(shows("never calibrated")); CHECK(shows("No calibrations yet"));
    CHECK(tap("Start calibration"));                  /* not logged in: refused, and why */
    CHECK(strlen(ui_toast()) > 0); CHECK_EQ(ui_screen(), UI_CAL);
    CHECK(tap("Log in")); CHECK(tap("Factory")); type("9999"); CHECK(tap("Enter"));
    CHECK(tap("System")); CHECK(tap("Calibrate"));
    CHECK(tap("Start calibration"));
    CHECK(shows("Start calibrating Zero gas?"));
    CHECK(tap("Yes"));
    cal_scan();
    unsigned ch = 9;
    CHECK(plat_cal_active(&ch)); CHECK_EQ(ch, 0u);
    CHECK(shows("calibrating now")); CHECK(shows("Accept"));
    CHECK(shows("CALIBRATION") || shows("FAULT"));    /* blank FRAM: CAL_INVALID's FAULT outranks CALIBRATION */
    /* the application's procedure: one point, then the new coefficient */
    CHECK(plat_cal_point(0, 0.0f, 1.2f, 0.1f));
    float coef[1] = { -1.1f };
    CHECK(plat_cal_propose(coef, 1));
    CHECK(tap("Accept")); CHECK(tap("Yes"));
    cal_scan();
    CHECK(!plat_cal_active(&ch));
    CHECK(fabsf(a_zero + 1.1f) < 1e-6f);
    CHECK(shows("In force: factory")); CHECK(shows("pass")); CHECK(shows("As found")); CHECK(shows("1.2"));
    dump("calibration");
    /* revert to factory: confirmed, then a REVERTED record */
    CHECK(tap("Revert to factory")); CHECK(shows("Put the factory calibration"));
    CHECK(tap("Yes"));
    cal_scan();
    CHECK(shows("reverted"));
    plat_cal_reset_for_test(); plat_state_reset_for_test();
}

/* System -> I/O boards: board states, lit outputs and inputs, faults in words */
static void iob_run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 20) {
        plat_io_in_t in; memset(&in, 0, sizeof in);
        plat_io_out_t out; memset(&out, 0, sizeof out);
        plat_do_set(&out, 1, true); plat_do_set(&out, 17, true);   /* output 2 and relay 2 on */
        iob_scan_in(&in, bsp_host.ms); iob_scan_out(&out, bsp_host.ms);
        iob_cycle(bsp_host.ms);
        bsp_host_advance_ms(20);
    }
}
static void t_iob_screen(void) {
    static iob_simbus_t bus; static iob_node_t nd;
    setup();
    iob_reset();
    iob_simbus_init(&bus); iob_node_init(&nd, 1); iob_simbus_add(&bus, &nd);
    iob_init(&bus.port);
    iob_link = IOB_LINK_RS485;
    CHECK(tap("System"));
    CHECK(tap("I/O boards"));
    CHECK_EQ(ui_screen(), UI_IOB);
    CHECK(shows("Board 1")); CHECK(shows("Board 3")); CHECK(shows("not fitted"));
    CHECK(shows("iob1_enable (110)"));
    iob_tags[0].enable = true;
    nd.inputs = 0x09;
    nd.a_open = 0x04;
    iob_run(100);
    frame();
    CHECK(shows("online"));
    CHECK(shows("RS-485, address 1"));
    CHECK(shows("Outputs 1-18")); CHECK(shows("Inputs 1-8"));
    CHECK(shows("Out 3: open circuit"));
    CHECK(shows("Firmware 1.0, hardware 1"));
    CHECK(!shows("T1 "));                             /* no temperature channel configured */
    /* map 2: configured temperature channels (calibrated) and the pump */
    iob_tags[0].temp_type[0] = IOB_TT_TC_K;
    nd.temp[0] = 2345; nd.temp_st[0] = IOB_TS_OK;
    nd.temp[1] = IOB_TEMP_INVALID; nd.temp_st[1] = IOB_TS_OPEN;
    iob_tags[0].temp_type[2] = IOB_TT_PT100_3W;
    nd.temp[4] = -125; nd.temp_st[4] = IOB_TS_OK;
    iob_tags[0].temp_offset[0] = 150;
    nd.tach_rpm = 1200;                               /* pump AO not assigned: duty 0 */
    iob_run(PLAT_IOB_TEMP_MS + 100);
    frame();
    CHECK(shows("T1 236.0 "));
    CHECK(shows("T2 fault")); CHECK(shows("Temp 2: open circuit"));
    CHECK(!shows("T3 ")); CHECK(!shows("T4 "));       /* connector 2 not configured */
    CHECK(shows("T5 -12.5 ")); CHECK(!shows("T6 "));  /* an RTD has no B channel */
    CHECK(shows("Pump 0.0 %, 1200 rpm"));
    /* on Ethernet: the board's IP and Modbus address */
    iob_link = IOB_LINK_ETH;
    iob_tags[0].ip = 0xC0A8013Du;
    frame();
    CHECK(shows("192.168.1.61, address 1"));
    dump("iob");
    iob_tags[0].ip = 0;
    frame();
    CHECK(shows("No IP set (iob1_ip)"));
    iob_tags[0].ip = 0xC0A8013Du;
    iob_link = IOB_LINK_RS485;
    iob_tags[0].temp_type[0] = iob_tags[0].temp_type[2] = IOB_TT_NONE;
    nd.powered = false;
    iob_run(1100);
    frame();
    CHECK(shows("not responding"));
    CHECK(shows("No reply: outputs off after the hold"));
    dump("iob_lost");
    CHECK(tap("Home"));
    CHECK_EQ(ui_screen(), UI_HOME);
    iob_tags[0].enable = false;
    iob_reset();
    iob_init(NULL);
    iob_link = IOB_LINK_ETH;
}

void test_ui_all(void) {
    bsp_host_reset();
    bsp_host.ms = 100000;
    RUN(t_eve_driver);
    RUN(t_fonts);
    RUN(t_ui);
    RUN(t_iob_screen);
    RUN(t_state_quality);
    RUN(t_cal_screen);
}
