/* plat_sys.c - the platform services the scan runs: parameter table and store. */
#include "plat_sys.h"
#include "param.h"
#include "param_store.h"

static int sys_apply(const plat_cmd_t *c) {
    param_err_t e = param_apply(c);
    if (e == PERR_NO_ID) return SCAN_CMD_UNKNOWN;
    return (int)e;
}

static void sys_pre_logic(uint32_t now_ms) {
    param_scan_service(now_ms);        /* remote-link watchdog: the logic sees it this scan */
    param_store_scan_service();        /* snapshot for the store task, or apply an import */
}

const scan_sys_t plat_sys = { .apply = sys_apply, .pre_logic = sys_pre_logic, .publish = param_publish };
