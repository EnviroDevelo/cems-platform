/* plat_sys.c - the platform services the scan runs: parameter table, store, alarms, state. */
#include "plat_sys.h"
#include "param.h"
#include "param_store.h"
#include "plat_log.h"
#include "plat_alarm.h"
#include "plat_clock.h"
#include "plat_state.h"
#include "plat_power.h"
#include "plat_cal.h"
#include "plat_service.h"
#include "modbus.h"
#include "emis_bind.h"

static int sys_apply(const plat_cmd_t *c) {
    param_err_t e = param_apply(c);
    if (e == PERR_NO_ID) return SCAN_CMD_UNKNOWN;
    return (int)e;
}

static void sys_pre_logic(uint32_t now_ms) {
    param_blob_drain();                /* queued block writes (text, buffers, arrays) */
    param_scan_service(now_ms);        /* remote-link watchdog: the logic sees it this scan */
    param_store_scan_service();        /* snapshot for the store task, or apply an import */
    plat_clock_scan();                 /* a requested clock setting (read at the next scan) */
    plat_log_scan(now_ms);             /* data-log intervals, clock-change events */
    alarm_scan(now_ms);                /* delays and states; conditions set by last scan's logic */
    plat_cal_scan(now_ms);             /* calibration timeout, due dates (before the state: it may end CALIBRATION) */
    plat_state_scan(now_ms);           /* instrument state and the quality of each measurement */
    emis_bind_scan(now_ms);            /* emissions calculations from those measurements (emis.h) */
    plat_power_scan(now_ms);           /* supply voltage, power-fail events */
    plat_service_scan(now_ms);         /* operating hours, service and test records, configuration checksum */
    mb_block_scan(now_ms);             /* the platform's Modbus block: heartbeat, link age, uptime */
}

const scan_sys_t plat_sys = { .apply = sys_apply, .io_in = alarm_io_in, .pre_logic = sys_pre_logic, .publish = param_publish,
                               .io_out = alarm_io_out };
