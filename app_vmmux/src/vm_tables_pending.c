/* vm_tables_pending.c - tables not generated yet (TEMPORARY).
 * Each one is replaced by its generated file in the order of APP_INTEGRATION.md
 * section 11: parameters + retained blocks (step 2), then alarms, Modbus map and
 * home tiles (step 4). Removed from the build as each arrives. */
#include "vm_app.h"

const param_def_t  *const vm_params = NULL;      const size_t vm_n_params = 0;
const retain_def_t *const vm_retain = NULL;      const size_t vm_n_retain = 0;
const alarm_def_t  *const vm_alarm_defs = NULL;  const size_t vm_n_alarms = 0;
const mb_map_t     *const vm_modbus = NULL;      const size_t vm_n_modbus = 0;
const uint16_t     *const vm_home_ids = NULL;    const size_t vm_n_home = 0;
