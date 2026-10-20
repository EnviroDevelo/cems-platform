/* vm_home.c - the touchscreen home tiles (up to 6, APP_INTEGRATION.md section 9).
 * What the old main screens show first: the active mode of each side and its remaining
 * time (ProcessTimer / ProcessTimerMUX .Preset hold the time left, 46e V10, 45b). The
 * application screens (valve pictures, sequence editor ...) wait for the core thread's
 * application-screen API (INTEGRATION.md, request R2). */
#include "vm_app.h"
#include "vm_params.h"
#include "vm_params_modbus.h"

static const uint16_t home[] = {
    VMMB_CURRENTMODE,              /* VM mode: 0 none, 1 Modbus, 2 schedule, 3 manual */
    VMMB_PROCESSTIMER_PRESET,      /* VM time left, interconnection 1 (ms) */
    VMMB_CURRENTMODEMUX,           /* MUX mode */
    VMMB_PROCESSTIMERMUX_PRESET,   /* MUX time left (ms) */
    VMP_NUMBEROFMUXVALVES,         /* MUX valve count */
};
const uint16_t *const vm_home_ids = home;
const size_t vm_n_home = sizeof home / sizeof home[0];
