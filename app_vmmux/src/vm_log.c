/* vm_log.c - data-log channels (APP_INTEGRATION.md section 8).
 *
 * The old LOG columns Col3..Col22 were AI0, AI1, RTD0, RTD1 and Modbus parameters
 * 4..19 (48_Logging). The Modbus "parameters" never carried real values (finding L2),
 * so only the four measured channels are logged. vm_app.c puts one sample per scan
 * with its quality; the platform averages over the log interval (parameter 30).
 * Units: the old project lets the user name the columns and set units on the screens;
 * the new names and units come from the parameter table once it is generated. */
#include "vm_app.h"

static const plat_log_chan_t chans[] = {
    { "AI1",  NULL, 2 },     /* AnalogSensor[0] (scaled, Low point..High point) */
    { "AI2",  NULL, 2 },     /* AnalogSensor[1] */
    { "RTD1", NULL, 1 },     /* TemperatureSensor[0] / 10 (0.1 degree in the original) */
    { "RTD2", NULL, 1 },     /* TemperatureSensor[1] / 10 */
};
const plat_log_chan_t *const vm_log_chans = chans;
const size_t vm_n_log_chans = sizeof chans / sizeof chans[0];
