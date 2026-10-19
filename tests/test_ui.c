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

static eve_sim_t sim;
static eve_t eve;
static plat_cmdq_t q;
static plat_seqlock_t lk;

/* an application table like the demo's */
static uint32_t a_delay = 2000, a_run = 5;
static bool a_valve, a_in;
static float a_gain = 1.5f;
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
};
static const uint16_t home_ids[] = { 1001, 1002, 1003, 1005 };
static const ui_cfg_t cfg = { .product = "CEMS Supervisor", .version = "0.5.0", .home_ids = home_ids, .n_home = 4 };

static uint32_t now = 100000;
static void scan(void) {
    plat_cmd_t c; while (plat_cmdq_pop(&q, &c)) (void)param_apply(&c);
    param_blob_drain(); alarm_scan(now); param_publish();
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
static bool row(const char *name) {
    for (int k = 0; k < 10 && eve_sim_tag_of(&sim, "Up") > 0 && !shows(name); ++k) tap("Up");
    if (!shows(name)) for (int k = 0; k < 10 && !shows(name); ++k) if (!tap("Down")) break;
    return tap(name);
}
static void type(const char *s) { for (; *s; ++s) press((uint8_t)*s); }
static void dump(const char *name) {
    const char *dir = getenv("UI_DUMP");
    if (!dir) return;
    char p[256]; snprintf(p, sizeof p, "%s/%s.eve", dir, name);
    FILE *f = fopen(p, "wb"); if (f) { eve_sim_dump(&sim, f); fclose(f); }
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
    CHECK(shows("Control: LOCAL (this touchscreen)"));
    dump("home");

    /* settings: everything readable; changes need a login */
    CHECK(tap("Settings"));
    CHECK_EQ(ui_screen(), UI_PARAMS);
    CHECK(shows("ctrl_mode")); CHECK(shows("comms_timeout")); CHECK(shows("LOCAL"));
    dump("settings");
    CHECK(row("start_delay"));
    CHECK_EQ(ui_screen(), UI_PARAMS);
    CHECK(strstr(ui_toast(), "Log in as operator") != NULL);

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
    dump("system");
    uint32_t swaps = fs.swaps;
    CHECK_EQ(eve_sim_tag_of(&sim, "Restart on 0.6.0"), 0);          /* disabled for admin */
    CHECK(tap("Log out admin"));
    CHECK(tap("Log in")); CHECK(tap("Factory")); type("9999"); CHECK(tap("Enter"));
    CHECK_EQ(ui_level(), PLAT_LVL_FACTORY);
    CHECK(tap("Restart on 0.6.0")); CHECK(shows("Restart on firmware 0.6.0 now?")); CHECK(tap("Yes"));
    fw_service(now, true);
    CHECK_EQ(fs.swaps, swaps + 1);
    CHECK(shows("80 %"));
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
    CHECK(tap("Acknowledge"));
    CHECK(!strcmp(ui_toast(), "Acknowledged"));
    CHECK(shows("2 ALARMS  1 NEW"));
    CHECK(tap("Ack all"));
    CHECK(shows("2 ALARMS"));
    CHECK(!shows("Acknowledge"));
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

void test_ui_all(void) {
    bsp_host_reset();
    bsp_host.ms = 100000;
    RUN(t_eve_driver);
    RUN(t_ui);
}
