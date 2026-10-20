/* vm_hmi.h - the old screens' actions as parameter-table entries (vm_hmi.c).
 * IDs are fixed (FRAM-free, but the UI, web page and any master use them):
 *   5000..5019  commands: write 1 (valve commands: the valve 1..19) = one press
 *   5100..5199  status shown by the old screens (read only) */
#ifndef VM_HMI_H
#define VM_HMI_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "param.h"

enum {   /* command index; parameter ID = VM_CMD_FIRST + index */
    VMC_VM_MODBUS = 0, VMC_VM_SCHEDULE, VMC_VM_MANUAL, VMC_VM_MANUAL_YES, VMC_VM_MANUAL_NO,
    VMC_VM_MANUAL_START, VMC_VM_INLET, VMC_VM_OUTLET, VMC_VM_SEQ_ABSENT_OK, VMC_VM_INCORRECT_OK,
    VMC_MUX_MODBUS, VMC_MUX_SCHEDULE, VMC_MUX_MANUAL, VMC_MUX_MANUAL_YES, VMC_MUX_MANUAL_NO,
    VMC_MUX_MANUAL_START, VMC_MUX_INLET, VMC_MUX_PURGE, VMC_MUX_SEQ_ABSENT_OK, VMC_MUX_INCORRECT_OK,
    VM_CMD_COUNT
};
enum { VM_CMD_FIRST = 5000 };

enum {   /* status parameter IDs */
    VMS_VM_MODBUS_STATE = 5100, VMS_VM_SCHEDULE_STATE, VMS_VM_MANUAL_STATE,
    VMS_VM_MODBUS_EN, VMS_VM_SCHEDULE_EN, VMS_VM_MANUAL_EN,
    VMS_VM_MANUAL_POPUP, VMS_VM_START_VISIBLE, VMS_VM_SEQ_ABSENT, VMS_VM_INCORRECT,
    VMS_VM_INLET_EN, VMS_VM_OUTLET_EN,
    VMS_MUX_MODBUS_STATE = 5120, VMS_MUX_SCHEDULE_STATE, VMS_MUX_MANUAL_STATE,
    VMS_MUX_MODBUS_EN, VMS_MUX_SCHEDULE_EN, VMS_MUX_MANUAL_EN,
    VMS_MUX_MANUAL_POPUP, VMS_MUX_START_VISIBLE, VMS_MUX_SELECT_EN, VMS_MUX_SEQ_ABSENT, VMS_MUX_INCORRECT,
    VMS_MUX_INLET_VISIBLE, VMS_MUX_INLET_ON, VMS_MUX_PURGE_ON,
};

extern uint8_t ManualMode, ManualModeMUX;               /* HMI-only tags of the old project */
extern bool    ResetManualAttemptMUX, StartButtonVisibMUX;
extern const param_def_t *const vm_hmi_params;
extern const size_t vm_n_hmi_params;
void vm_hmi_pre_logic(void);    /* entry emulation, then the presses: before Function1 */
void vm_hmi_post_logic(void);   /* after Function1 */
#endif
