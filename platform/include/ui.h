/* ui.h - the touchscreen (design spec "HMI design rules", "Control arbitration",
 * "Firmware update"). Same functions and data as the web page, drawn by the EVE.
 *
 * Screens: Home (value tiles, local/remote control), Settings (the whole parameter
 * table, paged; tap a row to change it), Edit (numeric keypad, IP entry, on/off,
 * text keyboard), Log in (operator/admin/factory, same passwords as the web),
 * Events (the event log, paged), System (firmware and update, network, unit,
 * storage, brightness), Alarms (reached from the alarm pill in the header: the
 * list, most urgent first, acknowledge one or all; acknowledging needs no login),
 * Alarm actions (from the Alarms screen: email / Modbus / relay 1-4 per alarm; admin).
 * A header shows the mode and the alarm count; a bar of five buttons along the
 * bottom changes screen.
 *
 * Rules carried over from the spec: touch targets at least 60 px; a confirm dialog
 * before the mode changes, the network settings change, defaults are restored,
 * firmware is activated, or any entry flagged PF_CONFIRM changes; the local
 * operator can always take control back (LOCAL needs no login); writes go through
 * the parameter table with source PLAT_SRC_LOCAL, so every check applies; the
 * screen dims after 10 minutes and the first touch only wakes it; a login lapses
 * after 10 minutes without activity.
 *
 * Threading: UI task only. Reads the published parameter values, posts writes. */
#ifndef UI_H
#define UI_H
#include <stdint.h>
#include <stdbool.h>
#include "eve.h"

typedef struct {
    const char     *product;     /* header title */
    const char     *version;     /* running firmware */
    const uint16_t *home_ids;    /* parameter IDs shown as tiles on the home screen (up to 6) */
    unsigned        n_home;
} ui_cfg_t;

enum { UI_HOME = 0, UI_PARAMS, UI_EDIT, UI_LOGIN, UI_EVENTS, UI_SYSTEM, UI_ALARMS, UI_ALARM_CFG, UI_CLOCK };

void    ui_init(const ui_cfg_t *cfg, uint32_t now_ms);
void    ui_input(uint8_t tag, uint32_t now_ms);     /* the tag under the finger now (0 = none); every 10..20 ms */
bool    ui_due(uint32_t now_ms);                     /* a redraw is needed (change, or 4 Hz refresh) */
void    ui_draw(eve_t *e, uint32_t now_ms);          /* build and send one frame */
uint8_t ui_brightness(uint32_t now_ms);              /* backlight %, dimmed when idle */

/* state, for tests */
int     ui_screen(void);
uint8_t ui_level(void);
const char *ui_toast(void);

/* The board's network status for the System screen (weak default: not known). */
bool ui_net_status(uint32_t *ip, bool *link_up, uint8_t mac[6]);

#endif
