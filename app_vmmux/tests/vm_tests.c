/* vm_tests.c - host tests of the VM/MUX application on the real platform scan engine,
 * parameter table and store (make -f app_vmmux/app.mk vm-test).
 *
 * The faithful logic itself is tested function by function by the Stage 4 tests
 * (stage4_tests/run.sh, with every vm_config.h switch at the original behaviour). These
 * tests cover what integration added: the I/O mapping, the owner's fixes behind the
 * switches, whole scans of Function1 + MUX through the platform, and the hooks. */
#include <string.h>
#include <math.h>
#include "tst.h"
#include "scan_engine.h"
#include "plat_sys.h"
#include "bsp_host.h"
#include "vm_app.h"
#include "vm_tags.h"
#include "vm_params.h"
#include "param_store.h"

int tst_fail, tst_pass;

static scan_engine_t s; static plat_cmdq_t q; static plat_seqlock_t lock;

static void power_up(void)
{
    const plat_app_t *a = vmmux_app();
    bsp_host_reset();
    plat_cmdq_init(&q);
    scan_init(&s, a->scan, &scan_hw_bsp, &q, &lock, 10);
    CHECK_EQ(param_init(a->params, a->n_params), PERR_OK);
    param_store_init(bsp_param_nvm(), NULL, a->retain, a->n_retain, a->layout_version, a->migrate);
    param_store_boot();
    alarm_init(a->alarms, a->n_alarms, BSP_RESET_POWER_ON);
    scan_set_sys(&s, &plat_sys);
    bsp_host.rtc_valid = true;
    bsp_host.rtc = (bsp_rtc_t){ .year = 2026, .month = 10, .day = 20, .hours = 9, .minutes = 0, .seconds = 0, .dow_iso = 2 };   /* Tuesday 20 Oct 2026 */
    for (unsigned c = 0; c < PLAT_AI_MAX; c++) bsp_host.in.ai_status[c] = PLAT_AIN_NOT_FITTED;
    for (unsigned c = 0; c < PLAT_RTD_MAX; c++) bsp_host.in.rtd_status[c] = PLAT_AIN_NOT_FITTED;
}

static void scans(int n)
{
    while (n-- > 0) { scan_step(&s); param_store_poll(bsp_ms()); bsp_host_advance_ms(10); }
}

/* ---------------- I/O conversions ---------------- */

static void t_ai_conversion(void)
{
    CHECK_EQ(vm_ai_to_ta30(0), 0);
    CHECK_EQ(vm_ai_to_ta30(-5), 0);
    CHECK_EQ(vm_ai_to_ta30(16383), 16383);
    CHECK_EQ(vm_ai_to_ta30(8192), 8192);
    CHECK_EQ(vm_ai_to_ta30(100000), 16383);           /* clamped to the TA30 range */
    CHECK_EQ(vm_ai_status_to_ta30(PLAT_AIN_OK), 0);
    CHECK_EQ(vm_ai_status_to_ta30(PLAT_AIN_OVER), 2);  /* the code the logic tests (10_IO_Map section 4) */
    CHECK_EQ(vm_ai_status_to_ta30(PLAT_AIN_UNDER), VM_AI_STATUS_FAULT);
    CHECK_EQ(vm_ai_status_to_ta30(PLAT_AIN_NOT_FITTED), VM_AI_STATUS_FAULT);
}

static void t_rtd_conversion(void)
{
    CHECK_EQ(vm_rtd_to_ta30(2534, PLAT_AIN_OK), 253);   /* 25.34 degC -> 25.3 */
    CHECK_EQ(vm_rtd_to_ta30(2535, PLAT_AIN_OK), 254);   /* rounds half away from zero */
    CHECK_EQ(vm_rtd_to_ta30(-1235, PLAT_AIN_OK), -124);
    CHECK_EQ(vm_rtd_to_ta30(0, PLAT_AIN_OK), 0);
    CHECK_EQ(vm_rtd_to_ta30(99999999, PLAT_AIN_OK), 32766);   /* never a fault code */
    CHECK_EQ(vm_rtd_to_ta30(0, PLAT_AIN_OVER), 32767);         /* TA30 fault values */
    CHECK_EQ(vm_rtd_to_ta30(0, PLAT_AIN_UNDER), -32767);
    CHECK_EQ(vm_rtd_to_ta30(0, PLAT_AIN_FAULT), -32768);
    CHECK_EQ(vm_rtd_to_ta30(2000, PLAT_AIN_NOT_FITTED), -32768);   /* not fitted is a fault, not a value */
}

/* ---------------- whole scans ---------------- */

static void t_runs_clean(void)
{
    /* One minute of scans from a blank unit: no sanitizer report, no out-of-bounds index
     * caught by the faithful code's checks, outputs all off. */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    g_oob = 0; g_div0 = 0;
    power_up();
    scans(6000);
    CHECK_EQ(scan_stats(&s).scans, 6000);
    /* DI_Control L45 loops i = 0..20 over the 20-element tmpDO / DOList (43b): one read and
     * one write outside each scan, skipped as A4.1 / A4.8. Expected, nothing else may add. */
    CHECK_EQ(g_oob, 2 * 6000);
    CHECK_EQ(g_div0, 0);
    CHECK_EQ(bsp_host.out.do_bits, 0u);
    CHECK_EQ(CurrentMode, 0);
    CHECK_EQ(CurrentModeMUX, 0);
}

static void t_di_mapping(void)
{
    /* VM_FIX_DI_REAL_INPUTS (open item 8): DIG_IN_01..20 = TA30 I0..I13, UID I0..I5 reach
     * DiscreteInputsArray[0..19] in the order of the disabled rungs 9-10. */
    power_up();
    for (unsigned n = 0; n < 20; n++) {
        unsigned ch = n < 14 ? n : 14u + (n - 14u);      /* platform DI channel */
        bsp_host.in.di = 1u << ch;
        scans(1);
        int on = 0, at = -1;
        for (int i = 0; i < 22; i++) if (DiscreteInputsArray[i]) { on++; at = i; }
        CHECK_EQ(on, 1);
        CHECK_EQ(at, (int)n);
    }
    bsp_host.in.di = 1u << 20;            /* UID I6: never read by the original (10_IO_Map section 3) */
    scans(1);
    int on = 0; for (int i = 0; i < 22; i++) on += DiscreteInputsArray[i];
    CHECK_EQ(on, 0);
    bsp_host.in.di = 0;
}

static void t_do_mapping(void)
{
    plat_io_out_t out; memset(&out, 0, sizeof out);
    memset(TA30_DO, 0, sizeof TA30_DO); memset(UID_DO, 0, sizeof UID_DO);
    TA30_DO[0] = true; TA30_DO[9] = true; UID_DO[0] = true; UID_DO[9] = true;
    vm_io_out(&out);
    CHECK_EQ(out.do_bits, (1u << 0) | (1u << 9) | (1u << 10) | (1u << 19));
    memset(TA30_DO, 0, sizeof TA30_DO); memset(UID_DO, 0, sizeof UID_DO);
}

static void t_purge_valve_reaches_output(void)
{
    /* End to end through Function1 -> MUX -> DO_AssignMUX / DO_ControlMUX -> DO_Control ->
     * TA30_DO -> platform output image: the purge inlet's valve (DOPurgeInlet, 1-based). */
    power_up();
    scans(2);
    DOPurgeInlet = 7;
    PurgeInletActivate = true;
    /* Function1 writes the outputs (DO_Control, rung 28) before it calls MUX (rung 36), so a
     * MUX valve reaches the output one scan later, as in the original (I-12). */
    scans(1);
    CHECK_EQ(bsp_host.out.do_bits, 0u);
    scans(1);
    CHECK_EQ(bsp_host.out.do_bits, 1u << 6);
    char why[64] = "";
    CHECK(!vm_fw_safe(why, sizeof why));                /* no firmware restart during a purge */
    CHECK(strstr(why, "purge") != NULL);
    PurgeInletActivate = false;
    scans(2);
    CHECK_EQ(bsp_host.out.do_bits, 0u);
    CHECK(vm_fw_safe(why, sizeof why));
    DOPurgeInlet = 0;
}

static void t_ai_scaling_fixed(void)
{
    /* VM_FIX_AI_SCALING (open item 9): Low point at raw 0, High point at full scale. */
    power_up();
    FirstCalibPoint[0] = 0.0f; SecondCalibPoint[0] = 100.0f;
    bsp_host.in.ai_status[0] = PLAT_AIN_OK;
    bsp_host.in.ai_raw[0] = 0;       scans(1); CHECK(fabsf(AnalogSensor[0] - 0.0f) < 1e-3f);
    bsp_host.in.ai_raw[0] = 16383;   scans(1); CHECK(fabsf(AnalogSensor[0] - 100.0f) < 1e-3f);
    bsp_host.in.ai_raw[0] = 4096;    scans(1); CHECK(fabsf(AnalogSensor[0] - 25.0015f) < 1e-2f);
    bsp_host.in.ai_status[0] = PLAT_AIN_OVER;   /* overflow forces the high end, as the original */
    scans(1); CHECK(fabsf(AnalogSensor[0] - 100.0f) < 1e-3f);
    bsp_host.in.ai_status[0] = PLAT_AIN_NOT_FITTED;
}

static void t_rtd_through_logic(void)
{
    power_up();
    TemperatureShift[0] = 5;                     /* +0.5 degree */
    bsp_host.in.rtd_status[0] = PLAT_AIN_OK;
    bsp_host.in.rtd_cdeg[0] = 2120;              /* 21.20 degC */
    scans(1);
    CHECK_EQ(TemperatureSensor[0], 212 + 5);
    bsp_host.in.rtd_status[0] = PLAT_AIN_NOT_FITTED;
    scans(1);
    CHECK_EQ(TA30_TEMP[0], -32768);              /* the TA30 connection-fault value */
    TemperatureShift[0] = 0;
}

static void t_fw_safe_modes(void)
{
    char why[64];
    CurrentMode = 2; CHECK(!vm_fw_safe(why, sizeof why)); CHECK(strstr(why, "Valve Manager schedule") != NULL);
    CurrentMode = 0; CurrentModeMUX = 3; CHECK(!vm_fw_safe(why, sizeof why)); CHECK(strstr(why, "Multiplexor manual") != NULL);
    CurrentModeMUX = 0; CHECK(vm_fw_safe(why, sizeof why));
}


/* ---------------- parameter table and store ---------------- */

static param_err_t write_i(uint16_t id, int32_t v)
{
    plat_cmd_t c = { .id = id, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_ADMIN, .kind = PLAT_VAL_I32, .v.i = v };
    param_err_t e = param_check(id, c.kind, c.v, c.src, c.level);
    if (e == PERR_OK) { plat_cmdq_post(&q, &c); scans(1); }
    return e;
}

static void t_store_fits_and_keeps(void)
{
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(200);                                   /* the store saves within about 1 s */
    store_stats_t st = param_store_stats();
    printf("  user image %u bytes (32 KB FRAM map: 14848 per copy), %u parameters, %u blocks, status 0x%04x\n",
           (unsigned)st.payload_usr, (unsigned)vm_n_params, (unsigned)vm_n_retain, param_store_status());
    CHECK(!(param_store_status() & PSTAT_TABLE_ERROR));
    CHECK(st.payload_usr > 0);
    CHECK(st.payload_usr <= 14848u);

    /* a setting written through the table survives a power cycle */
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 12), PERR_OK);
    CHECK_EQ(MaxNumberOfValves, 12);
    /* the panel's limits: 2..20, and NumberOfMUXValves 2..MaxNumberOfValves (interlock at apply) */
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 21), PERR_RANGE);
    write_i(VMP_MAXNUMBEROFVALVES, 1);
    CHECK_EQ(MaxNumberOfValves, 12);              /* refused by the interlock */
    write_i(VMP_NUMBEROFMUXVALVES, 13);
    CHECK(NumberOfMUXValves != 13);               /* above MaxNumberOfValves: refused (the logic itself may adjust it) */
    write_i(VMP_NUMBEROFMUXVALVES, 8);
    CHECK_EQ(NumberOfMUXValves, 8);
    strcpy(SingularInlet[2], "Stack B");          /* a retained name the logic or UI changed */
    scans(200);
    MaxNumberOfValves = 0; NumberOfMUXValves = 0; memset(SingularInlet[2], 0, sizeof SingularInlet[2]);
    power_up();
    CHECK_EQ(MaxNumberOfValves, 12);
    CHECK_EQ(NumberOfMUXValves, 8);
    CHECK(strcmp(SingularInlet[2], "Stack B") == 0);
}

static void t_power_up_defaults(void)
{
    /* blank FRAM: retained blocks and texts come back to the original power-up values, not zero */
    ModbusTimer2.Preset = 1; memset(ParamColName[0], 0, sizeof ParamColName[0]);
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    CHECK_EQ(ModbusTimer2.Preset, 7200000u);
    CHECK(strcmp(ParamColName[0], "Param1") == 0);
    CHECK_EQ(LoggingIndex, 1);
}

int main(void)
{
    RUN(t_ai_conversion);
    RUN(t_rtd_conversion);
    RUN(t_do_mapping);
    RUN(t_runs_clean);
    RUN(t_di_mapping);
    RUN(t_purge_valve_reaches_output);
    RUN(t_ai_scaling_fixed);
    RUN(t_rtd_through_logic);
    RUN(t_fw_safe_modes);
    RUN(t_store_fits_and_keeps);
    RUN(t_power_up_defaults);
    printf("\nvm_tests: %d checks passed, %d failed\n", tst_pass, tst_fail);
    return tst_fail ? 1 : 0;
}
