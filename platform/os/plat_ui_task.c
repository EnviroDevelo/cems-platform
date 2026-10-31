/* plat_ui_task.c - the UI task: owns the EVE display. Replaces the weak default
 * body in plat_tasks_freertos.c.
 *
 * Every 20 ms: read the touch tag, pass it to the UI, redraw when something
 * changed (or at 4 Hz for live values), set the backlight. If the display does not
 * answer, it is retried every 5 s and the instrument carries on without it; a
 * display that stops answering (cable, reset) is noticed within 2 s.
 *
 * Supply failing (plat_power_failing, CIP-07.5): the backlight goes off within about 2 ms
 * and nothing more is sent or read until the supply is back (the hold-up doubles with the
 * backlight off, CORE_PCB.md 5a). The last frame stays in the BT817, so restoring the
 * backlight shows it at once; the 4 Hz refresh follows. */
#include "FreeRTOS.h"
#include "task.h"
#include "plat_tasks.h"
#include "ui.h"
#include "eve.h"
#include "bsp.h"
#include "plat_power.h"
#include "plat_role.h"

static eve_t eve;
static volatile bool disp_up, disp_fitted;
static volatile uint32_t disp_frames_at;

/* healthy for the firmware trial: the display works, or none is fitted */
bool plat_ui_healthy(void) { return !disp_fitted || disp_up; }
bool plat_ui_fitted(void) { return disp_fitted; }

void plat_ui_task_body(void) {
    if (plat_role() == PLAT_ROLE_NODE)               /* an I/O board has no display */
        for (;;) { plat_checkin(PLAT_TASK_UI); vTaskDelay(pdMS_TO_TICKS(100)); }
    eve_bus_t *bus = bsp_eve_bus();
    const plat_app_t *app = plat_app();
    static ui_cfg_t cfg;
    cfg.product = app->ident.product;
    cfg.version = app->ident.revision;
    cfg.home_ids = app->ui_home_ids;
    cfg.n_home = (unsigned)app->n_ui_home;
    disp_fitted = bus != NULL;
    ui_init(&cfg, bsp_ms());
    uint32_t retry = 0, check = 0;
    uint8_t bl = 255;
    bool first = true, dark = false;
    for (;;) {
        plat_checkin(PLAT_TASK_UI);
        uint32_t now = bsp_ms();
        if (!bus) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (plat_power_failing()) {
            if (!dark && disp_up) eve_backlight(&eve, 0);
            dark = true;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (dark) { dark = false; bl = 255; }             /* back: the backlight is set again below */
        if (!disp_up) {
            if (first || now - retry >= 5000u) {
                first = false;
                retry = now;
                disp_up = eve_init(&eve, bus, &eve_panel_rvt70h) == 0;
                if (disp_up) { plat_checkin(PLAT_TASK_UI); (void)eve_fonts_load(&eve, EVE_FONT_BASE); }   /* own fonts; ROM fonts if this fails */
                bl = 255;
                plat_checkin(PLAT_TASK_UI);
            }
            if (!disp_up) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        }
        ui_input(eve_touch_tag(&eve), now);
        if (ui_due(now) && !plat_power_failing()) ui_draw(&eve, now);   /* checked again before the long transfer */
        uint8_t b = ui_brightness(now);
        if (b != bl) { eve_backlight(&eve, b); bl = b; }
        if (now - check >= 2000u) {
            check = now;
            if (eve_rd8(&eve, REG_ID) != EVE_CHIP_ID || eve.timeouts > 10u) disp_up = false;   /* gone: start again */
            disp_frames_at = eve.frames;
        }
        for (int k = 0; k < 10 && !plat_power_failing(); ++k) vTaskDelay(pdMS_TO_TICKS(2));   /* 20 ms, watching the supply */
    }
}
