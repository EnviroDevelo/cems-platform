/* vm_ui.h - the application's screens and record editors for the platform (ui_app.h, D26),
 * and the read-only display values they need (vm_ui.c).
 *
 * IDs 5150..5159 (display only, never stored, never on Modbus):
 *   5150..5152  time left in seconds: VM tube 1, VM tube 2, MUX step
 *   5153..5156  VM valve row: inlet shown / inlet open / outlet shown / outlet open, per
 *               picture position 1..19 (tube 1 from the left, tube 2 from the right, 4F1)
 *   5157        number of picture positions in use (9 or 19)
 *   5158, 5159  RTD1 / RTD2 in degrees (the tags hold 0.1 degree) */
#ifndef VM_UI_H
#define VM_UI_H
#include <stddef.h>
#include "param.h"
#include "ui_app.h"

enum {
    VMU_LEFT1_S = 5150, VMU_LEFT2_S, VMU_LEFTMUX_S,
    VMU_IN_SHOWN, VMU_IN_OPEN, VMU_OUT_SHOWN, VMU_OUT_OPEN,
    VMU_POSITIONS, VMU_RTD1, VMU_RTD2,
};

extern const ui_app_t vm_ui;
extern const param_def_t *const vm_ui_params;
extern const size_t vm_n_ui_params;
void vm_ui_pre_logic(void);    /* Touched_Bit from the panel's last touch (R3) */
void vm_ui_scan(void);         /* refresh the display values: once per scan after the logic */
#endif
