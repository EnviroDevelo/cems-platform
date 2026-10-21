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
#include "vm_alarms.h"
#include "plat_alarm.h"
#include "modbus.h"
#include "vm_hmi.h"
#include "vm_records.h"

int tst_fail, tst_pass;

static scan_engine_t s; static plat_cmdq_t q; static plat_seqlock_t lock;

static void power_up(void)
{
    const plat_app_t *a = vmmux_app();
    bsp_host_reset();
    plat_cmdq_init(&q);
    param_bind(&q, NULL);                         /* Modbus / web writes go to this scan's queue */
    scan_init(&s, a->scan, &scan_hw_bsp, &q, &lock, 10);
    CHECK_EQ(param_init(a->params, a->n_params), PERR_OK);
    param_store_init(bsp_param_nvm(), NULL, a->retain, a->n_retain, a->layout_version, a->migrate);
    param_store_boot();
    CHECK_EQ(alarm_init(a->alarms, a->n_alarms, BSP_RESET_POWER_ON), 0);
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

/* ---------------- alarms ---------------- */

static void t_alarms(void)
{
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    /* DI 4 configured as an alarm on TRUE (VM DI_Control) -> DI_ALARM_04 */
    Di_FunctionVM[3] = 1; AlarmEnableVM[3] = true; AlarmConditionVM[3] = 1;
    bsp_host.in.di = 1u << 3;
    /* DI_Control runs before DI_Assigning copies the inputs (original order), and the platform
     * evaluates conditions at the start of the next scan: raised on the third scan (30 ms) */
    scans(3);
    CHECK(AlarmsVM[3]);
    CHECK(alarm_active(VM_AL_DI_FIRST + 3));
    bsp_host.in.di = 0; scans(3);
    CHECK(!AlarmsVM[3]);
    CHECK(!alarm_active(VM_AL_DI_FIRST + 3));           /* not latched: clears with its condition */
    alarm_view_t av; CHECK(alarm_get(VM_AL_DI_FIRST + 3, &av)); CHECK(av.unacked);   /* waits for an ack */
    Di_FunctionVM[3] = 0; AlarmEnableVM[3] = false;

    /* enabled analog channel with no sensor: fault alarm; disabled: none (bench: NOT_FITTED) */
    CHECK(!alarm_active(VM_AL_AI1_FAULT));
    AnalogEnable[0] = true; scans(2);
    CHECK(alarm_active(VM_AL_AI1_FAULT));
    AnalogEnable[0] = false; scans(2);

    /* flow validation: latched by the platform; the old FlowValidationAlarm bit follows it */
    FlowValidationTimer.Out = true;  /* the MUX rung-18 timer output, forced here */
    vm_alarms_scan();
    FlowValidationAlarm = true;      /* what MUX rung 18 does on the rising edge */
    scans(3);
    CHECK(!FlowValidationTimer.Out);                    /* the condition has gone ... */
    CHECK(alarm_active(VM_AL_FLOW_VALIDATION));         /* ... the alarm stays (AL_LATCH) */
    CHECK(FlowValidationAlarm);                         /* and so does the old bit */
    plat_cmd_t c = { .id = PARAM_ID_ALARM_ACK, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_VIEW,
                     .kind = PLAT_VAL_U32, .v.u = VM_AL_FLOW_VALIDATION };
    CHECK_EQ(param_check(c.id, c.kind, c.v, c.src, c.level), PERR_OK);   /* no login needed (D19) */
    plat_cmdq_post(&q, &c);
    scans(3);
    CHECK(!alarm_active(VM_AL_FLOW_VALIDATION));
    CHECK(!FlowValidationAlarm);                        /* old Email rung 20 behaviour, via the platform ack */
}

/* ---------------- Modbus: the old slave map at its old addresses ---------------- */

static uint16_t mb(const uint8_t *req, uint16_t len, uint8_t *resp)
{
    return mb_pdu(req, len, resp, 256);
}

static void t_modbus(void)
{
    const plat_app_t *a = vmmux_app();
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    CHECK_EQ(mb_init(a->modbus_map, a->n_modbus_map, &a->ident), 0);
    scans(2);
    uint8_t r[256];

    /* holding 0 = CurrentMode, 1 = NumInterconnections (FC3) */
    NumInterconnections = 2; scans(1);
    const uint8_t rd[] = { 3, 0, 0, 0, 2 };
    CHECK_EQ(mb(rd, sizeof rd, r), 6);
    CHECK_EQ(r[0], 3); CHECK_EQ(((r[2] << 8) | r[3]), 0); CHECK_EQ(((r[4] << 8) | r[5]), NumInterconnections);   /* whatever the logic made of it */   /* fc, byte count, data */

    /* coils 13..34 = AlarmsVM[0..21] */
    AlarmsVM[2] = true; Di_FunctionVM[2] = 1; AlarmEnableVM[2] = true; AlarmConditionVM[2] = 0;  /* alarm on FALSE */
    scans(3);
    const uint8_t rc[] = { 1, 0, 13, 0, 22 };
    CHECK_EQ(mb(rc, sizeof rc, r), 5);
    CHECK_EQ((r[2] & 0x04), 0x04);
    Di_FunctionVM[2] = 0; AlarmEnableVM[2] = false;

    /* a master write in LOCAL mode is refused (04); in REMOTE it reaches the tag */
    const uint8_t wc[] = { 5, 0, 7, 0xFF, 0 };   /* coil 7 StartModbusCoilMUX */
    CHECK_EQ(mb(wc, sizeof wc, r), 2); CHECK_EQ(r[0], 0x85); CHECK_EQ(r[1], 4);
    plat_cmd_t c = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR,
                     .kind = PLAT_VAL_U32, .v.u = PARAM_MODE_REMOTE };
    plat_cmdq_post(&q, &c); scans(1);
    const uint8_t wr[] = { 6, 0x02, 0xFA, 0, 3 };   /* holding 762 SelectedValveMbMUX = 3 */
    CHECK_EQ(mb(wr, sizeof wr, r), 5); CHECK_EQ(r[0], 6);
    scans(1);
    CHECK_EQ(SelectedValveMbMUX, 3);

    /* a register the master only reads is refused for writing (02) */
    const uint8_t wro[] = { 6, 0, 0, 0, 1 };
    CHECK_EQ(mb(wro, sizeof wro, r), 2); CHECK_EQ(r[1], 2);

    /* coil 91 "Acknowledge Bit" acknowledges every alarm (old Email rung 20) */
    AnalogEnable[0] = true; scans(3);            /* AI1 not fitted: sensor-fault alarm */
    alarm_view_t av; CHECK(alarm_get(VM_AL_AI1_FAULT, &av)); CHECK(av.unacked);
    const uint8_t ack[] = { 5, 0, 91, 0xFF, 0 };
    CHECK_EQ(mb(ack, sizeof ack, r), 5);
    scans(2);
    CHECK(alarm_get(VM_AL_AI1_FAULT, &av)); CHECK(!av.unacked);
    CHECK(!AcknowledgeBit);
    AnalogEnable[0] = false;

    /* description buffer of DI alarm 1 (registers 973..1002), first character */
    vmx_Alarm0_Description[0] = 'A'; vmx_Alarm0_Description[1] = 'B'; scans(1);
    const uint8_t rb[] = { 3, 0x03, 0xCD, 0, 2 };
    CHECK_EQ(mb(rb, sizeof rb, r), 6);
    CHECK_EQ(r[2], 'A'); CHECK_EQ(r[3], 'B'); CHECK_EQ(r[4], 0);   /* Q3: UTF-8, first byte in the high half */
    /* names on Modbus: the MUX inlet names (M8 fix) and the VM names (rebuilt when a name changes) */
    strcpy(SingularInlet[1], "Stack B"); scans(2);
    const uint8_t rn[] = { 3, 0x03, 0x09, 0, 2 };                     /* holding 777: ValveNameMUX2 */
    CHECK_EQ(mb(rn, sizeof rn, r), 6);
#if VM_FIX_MUX_MODBUS_NAMES
    CHECK(r[2] == 'S' && r[3] == 't' && r[4] == 'a' && r[5] == 'c');
#endif
    c.v.u = PARAM_MODE_LOCAL; plat_cmdq_post(&q, &c); scans(1);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMINTERCONNECTIONS, 1), PERR_OK);
    CHECK_EQ(write_i(VMP_FIRSTINLETS, 2), PERR_OK); CHECK_EQ(write_i(VMP_FIRSTOUTLETS, 1), PERR_OK);
    scans(5);
    /* inlet 2 and outlet 1 left blank: with the M10 fix they show their numbers in their own buffers */
    CHECK_EQ(param_blob_post(VMP_INLETNAME0, 0, "ZERO", 4, true, PLAT_SRC_LOCAL, PLAT_LVL_OPERATOR), PERR_OK);
    scans(20);                                                        /* EntryOK -> ChangeModbusNamesVM -> 47b */
    const uint8_t rv[] = { 3, 0, 20, 0, 2 };                          /* holding 20: FirstInterconInletName1 */
    CHECK_EQ(mb(rv, sizeof rv, r), 6);
#if VM_FIX_VM_MODBUS_NAMES
    CHECK(r[2] == 'Z' && r[3] == 'E' && r[4] == 'R' && r[5] == 'O');
    CHECK(FirstInterconInletName[1][0] == '2');                       /* blank inlet 2 -> "2" */
    CHECK(FirstInterconOutletName[0][0] == '1');                      /* blank outlet 1 -> "1" */
#endif
}

/* ---------------- owner decisions of 20 Oct ---------------- */

static void set_mode(uint32_t m)
{
    plat_cmd_t c = { .id = PARAM_ID_CTRL_MODE, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR,
                     .kind = PLAT_VAL_U32, .v.u = m };
    plat_cmdq_post(&q, &c); scans(1);
}

static void t_q1_link_loss(void)
{
    /* VM run started over Modbus ends after vm_link_close_min of link loss; MUX carries on */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    vm_link_close_min = 1;
    set_mode(PARAM_MODE_REMOTE);                 /* no Modbus or web traffic from here on */
    StartModbus = true; StartModbusMUX = true;
    scans(1100);                                 /* just over 10 s: the platform comms timeout */
    CHECK(param_remote_link_lost());
    CHECK(StartModbus);                          /* not yet: 1 minute of loss */
    scans(6000);
    CHECK(!StartModbus);                         /* VM run ended: its valves close */
    CHECK_EQ(SelectedFirstInlet, 0); CHECK_EQ(SelectedFirstOutlet, 0);
    CHECK(StartModbusMUX);                       /* the MUX loop is untouched */
    StartModbusMUX = false; vm_link_close_min = 30;
    set_mode(PARAM_MODE_LOCAL);
}

static void t_q5_q6_retained(void)
{
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    AnalogEnable[0] = true; AnalogAlarmEnabled[0] = true; RangeMin[0] = 1.5f; RangeMax[0] = 9.5f;
    TemperatureEnable[1] = true; TemperatureAlarmEnabled[1] = true; MaxTemperatureRange[1] = 850;
    vm_rtd_hyst[1] = 5.0f;
    FirstCalibPoint[0] = 4.0f; SecondCalibPoint[0] = 20.0f;
    scans(200);
    memset(AnalogEnable, 0, sizeof AnalogEnable); memset(RangeMin, 0, sizeof RangeMin);
    MaxTemperatureRange[1] = 0; vm_rtd_hyst[1] = 0; FirstCalibPoint[0] = 0;
    power_up();
    CHECK(AnalogEnable[0]); CHECK(AnalogAlarmEnabled[0]);       /* Q5: kept through power-off */
    CHECK(RangeMin[0] == 1.5f); CHECK(RangeMax[0] == 9.5f);
    CHECK(TemperatureEnable[1]); CHECK_EQ(MaxTemperatureRange[1], 850);
    CHECK(vm_rtd_hyst[1] == 5.0f);
    CHECK(FirstCalibPoint[0] == 4.0f);
    /* Q6: "restore defaults" resets user settings but never the calibration points */
    plat_cmd_t c = { .id = PARAM_ID_RESTORE_DEFAULTS, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_ADMIN,
                     .kind = PLAT_VAL_U32, .v.u = 1 };
    CHECK_EQ(param_check(c.id, c.kind, c.v, c.src, c.level), PERR_OK);
    plat_cmdq_post(&q, &c); scans(2);
    CHECK(!AnalogEnable[0]);                     /* user setting back to its default */
    CHECK(FirstCalibPoint[0] == 4.0f); CHECK(SecondCalibPoint[0] == 20.0f);
}

/* ---------------- operating the machine through the screen commands (vm_hmi.c) ---------------- */

static param_err_t press(unsigned c, uint32_t v)
{
    uint16_t id = (uint16_t)(VM_CMD_FIRST + c);
    plat_cmd_t k = { .id = id, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_OPERATOR, .kind = PLAT_VAL_U32, .v.u = v };
    param_err_t e = param_check(id, k.kind, k.v, k.src, k.level);
    if (e == PERR_OK) { plat_cmdq_post(&q, &k); scans(1); }
    return e;
}

static void write_i16_array(uint16_t id, const int16_t *v, uint16_t n)
{
    CHECK_EQ(param_blob_post(id, 0, v, (uint16_t)(n * 2u), false, PLAT_SRC_LOCAL, PLAT_LVL_ADMIN), PERR_OK);
    scans(1);
}

static void t_mux_manual_run(void)
{
    /* configure a 4-inlet multiplexor as an engineer would on the touchscreen */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMBEROFMUXVALVES, 4), PERR_OK);
    scans(2);
    CHECK_EQ(NumberOfMUXValves, 4);
    /* AdjustNumberOfValvesMUX ran (EntryOK emulation): 4 valves = inlets 1..3 shown (42b: i < n) */
    CHECK(SingularInletVisib[2]); CHECK(!SingularInletVisib[3]);
    const int16_t map[4] = { 5, 6, 7, 8 };                              /* inlet 1..4 -> DO5..DO8 (1-based) */
    write_i16_array(VMP_DOSINGULARINLET, map, 4);

    /* the inlet buttons are refused until manual mode is entered */
    CHECK_EQ(press(VMC_MUX_INLET, 2), PERR_OK);                         /* check passes ... */
    scans(2);
    CHECK(!SingularInletActivate[1]);                                   /* ... the interlock refused it at apply */

    /* Manual mode -> popup -> Yes -> Start -> inlet 2 */
    CHECK(ManualModeEnableMUX);
    press(VMC_MUX_MANUAL, 1);  scans(1); CHECK(ResetManualAttemptMUX);
    press(VMC_MUX_MANUAL_YES, 1); scans(1); CHECK(StartButtonVisibMUX);
    /* the popup's SetBit(EnableSingulManualSelection) is overwritten by MUX rung 10 on the next scan
     * (it follows the manual run), so selection really opens with Start: as in the original */
    press(VMC_MUX_MANUAL_START, 1); scans(1); CHECK(ManualModeStartMUX); CHECK(EnableSingulManualSelection);
    press(VMC_MUX_INLET, 2); scans(3);
    CHECK(SingularInletActivate[1]);
    CHECK_EQ(bsp_host.out.do_bits, 1u << 5);                            /* DO6 (0-based 5) open */
    CHECK_EQ(CurrentModeMUX, 3);
    char why[64]; CHECK(!vm_fw_safe(why, sizeof why));                  /* no firmware restart now */

    /* inlet 4 is not shown with 4 valves: refused */
    press(VMC_MUX_INLET, 4); scans(2); CHECK(!SingularInletActivate[3]);

    /* toggle inlet 2 off, leave manual mode */
    press(VMC_MUX_INLET, 2); scans(3);
    CHECK_EQ(bsp_host.out.do_bits, 0u);
    press(VMC_MUX_MANUAL, 1); scans(2);
    CHECK(!ManualModeStartMUX);
    CHECK(vm_fw_safe(why, sizeof why));
}

static void t_vm_modbus_run_and_link_loss(void)
{
    /* A one-tube valve manager: 3 inlets on DO1..3, 2 outlets on DO4..5 */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    const plat_app_t *a = vmmux_app();
    CHECK_EQ(mb_init(a->modbus_map, a->n_modbus_map, &a->ident), 0);
    scans(2);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMINTERCONNECTIONS, 1), PERR_OK);
    scans(2);
    CHECK_EQ(write_i(VMP_FIRSTINLETS, 3), PERR_OK);
    CHECK_EQ(write_i(VMP_FIRSTOUTLETS, 2), PERR_OK);
    scans(2);
    /* DOInlet / DOOutlet hold 0-based outputs, indexed by the valve's position in one sequence:
     * inlets first, then outlets (43c: DOOutlet[FirstInlets + SecondInlets + k]) */
    const int16_t din[3] = { 0, 1, 2 }, dout[5] = { 0, 0, 0, 3, 4 };
    write_i16_array(VMP_DOINLET, din, 3);
    write_i16_array(VMP_DOOUTLET, dout, 5);
    scans(5);
    CHECK_EQ(FirstInlets, 3); CHECK_EQ(FirstOutlets, 2); CHECK(ModbusModeEnable);

    /* the operator puts the VM in Modbus mode on the touchscreen, then hands over to the PLC */
    CHECK_EQ(press(VMC_VM_MODBUS, 1), PERR_OK);
    scans(2);
    CHECK(StartModbus); CHECK_EQ(CurrentMode, 1);
    vm_link_close_min = 1;
    set_mode(PARAM_MODE_REMOTE);
    uint8_t r[256];
    const uint8_t t[] = { 16, 0, 10, 0, 2, 4, 0x00, 0x36, 0xEE, 0x80 };   /* ModbusTimer.Preset = 3,600,000 ms */
    CHECK_EQ(mb(t, sizeof t, r), 5);
    const uint8_t route[] = { 16, 0, 6, 0, 2, 4, 0, 2, 0, 1 };           /* inlet 2, outlet 1 */
    CHECK_EQ(mb(route, sizeof route, r), 5);
    scans(1);
    const uint8_t go[] = { 5, 0, 1, 0xFF, 0 };                          /* coil 1 StartModbusCoil */
    CHECK_EQ(mb(go, sizeof go, r), 5);
    scans(5);
    /* the selections are cleared at the end of every scan (Function1 R37, V21), so the route
     * shows only in the outputs: inlet 2 = DO2, outlet 1 = DO4 (0-based 1 and 3) */
    CHECK(!ValveSelectionErrorModbus);
    CHECK_EQ(bsp_host.out.do_bits, (1u << 1) | (1u << 3));
    uint32_t open_bits = bsp_host.out.do_bits;

    /* the PLC goes quiet: link lost after 10 s, valves held for vm_link_close_min, then closed */
    scans(1100 + 3000);
    CHECK(param_remote_link_lost());
    CHECK_EQ(bsp_host.out.do_bits, open_bits);                          /* 40 s in: still open */
    scans(3000);
    CHECK(!StartModbus);
    CHECK_EQ(bsp_host.out.do_bits, 0u);                                 /* Q1: every VM valve closed */
    vm_link_close_min = 30;
    set_mode(PARAM_MODE_LOCAL);
}

static void t_mux_schedule_run(void)
{
    /* the automated sequence: inlet 2 then inlet 3, dwell 2 s + sample 3 s each */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMBEROFMUXVALVES, 4), PERR_OK);
    const int16_t map[3] = { 5, 6, 7 };                              /* inlet 1..3 -> DO5..DO7 (1-based) */
    write_i16_array(VMP_DOSINGULARINLET, map, 3);
    memset(&SequenceMUX, 0, sizeof SequenceMUX);                      /* as saved by the sequence editor */
    SequenceMUX.NumberOfPeriods = 2;
    SequenceMUX.NumberArray[0] = 1; SequenceMUX.NumberArray[1] = 2;  /* rows past the last period are 0 */
    /* SingulValves 1 and 2 open SingularInletActivate[1] and [2] = inlets 2 and 3; value 0 opens
     * nothing, so inlet 1 cannot be scheduled (finding S8, open item 12) */
    SequenceMUX.SingulValves[0] = 1; SequenceMUX.SingulValves[1] = 2;
    SequenceMUX.DwellTimer[0] = SequenceMUX.DwellTimer[1] = 2000;
    SequenceMUX.SampleTimer[0] = SequenceMUX.SampleTimer[1] = 3000;
    memset(&ScheduleMUX, 0, sizeof ScheduleMUX);
#if !VM_MUX_CONTINUOUS
    ScheduleMUX.To_Hour[2] = 23; ScheduleMUX.To_Hour[1] = 59; ScheduleMUX.To_Hour[0] = 59;   /* every day, all day */
#endif   /* continuous (Gary 20 Oct): the calendar is not used; an empty window must not matter */
    scans(2);
    CHECK_EQ(press(VMC_MUX_SCHEDULE, 1), PERR_OK);
    uint32_t seen[8]; int when[8], n = 0; uint32_t last = 0xFFFFFFFFu;
    for (int t = 0; t < 2500; t++) {
        scans(1);
        if (bsp_host.out.do_bits != last && n < 8) { last = bsp_host.out.do_bits; seen[n] = last; when[n++] = t * 10; }
    }
    CHECK(n >= 3);
    CHECK_EQ(seen[0], 1u << 5);  CHECK(when[0] <= 20);                 /* inlet 2 (DO6) at the start */
    CHECK_EQ(seen[1], 1u << 6);  CHECK(when[1] >= 4900 && when[1] <= 5050);   /* inlet 3 after 2 s + 3 s */
    CHECK_EQ(seen[2], 0u);       CHECK(when[2] >= 9900 && when[2] <= 10050);  /* end of the sequence */
    CHECK_EQ(CurrentModeMUX, 2);
    /* the loop (VM_MUX_CONTINUOUS, and F5 fixed by VM_FIX_MUX_RESTART): the sequence starts again
     * from channel 1 one scan after the end, until stopped */
#if VM_FIX_MUX_RESTART
    CHECK(n >= 6);
    CHECK_EQ(seen[3], 1u << 5); CHECK(when[3] - when[2] <= 30);
    CHECK_EQ(seen[4], 1u << 6); CHECK(when[4] - when[3] >= 4900 && when[4] - when[3] <= 5050);
#else
    CHECK_EQ(n, 3);                                                  /* the original stands still */
#endif
    CHECK(StartPeriodsMUX);
    press(VMC_MUX_SCHEDULE, 1); scans(3);
    CHECK(!StartPeriodsMUX);
    CHECK_EQ(bsp_host.out.do_bits, 0u);
}

static void t_mux_master_enable(void)
{
    /* Gary 20 Oct: the whole MUX feature can be switched off (mux_enable, parameter 2906) */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    CHECK(vm_mux_enable);                                              /* default on */
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMBEROFMUXVALVES, 4), PERR_OK);
    const int16_t map[3] = { 5, 6, 7 };
    write_i16_array(VMP_DOSINGULARINLET, map, 3);
    press(VMC_MUX_MANUAL, 1); press(VMC_MUX_MANUAL_YES, 1); press(VMC_MUX_MANUAL_START, 1);
    press(VMC_MUX_INLET, 1); scans(3);
    CHECK_EQ(bsp_host.out.do_bits, 1u << 4);                           /* inlet 1 open (DO5) */

    plat_cmd_t c = { .id = VM_PARAM_MUX_ENABLE, .src = PLAT_SRC_LOCAL, .level = PLAT_LVL_ADMIN,
                     .kind = PLAT_VAL_BOOL, .v.b = false };
    CHECK_EQ(param_check(c.id, c.kind, c.v, c.src, c.level), PERR_OK);
    plat_cmdq_post(&q, &c); scans(3);
    CHECK(!vm_mux_enable);
    CHECK_EQ(bsp_host.out.do_bits, 0u);                                /* every MUX valve closed */
    CHECK(!ManualModeStartMUX); CHECK_EQ(CurrentModeMUX, 0);
    /* the MUX buttons are refused while it is off; the VM is unaffected */
    press(VMC_MUX_SCHEDULE, 1); scans(2); CHECK(!StartScheduleMUX);
    press(VMC_MUX_MANUAL, 1);   scans(2); CHECK(!ResetManualAttemptMUX);
    /* kept through power-off, and on again from rest */
    scans(200); power_up(); scans(2);
    CHECK(!vm_mux_enable);
    c.v.b = true; plat_cmdq_post(&q, &c); scans(3);
    CHECK(vm_mux_enable);
    CHECK_EQ(bsp_host.out.do_bits, 0u);                                /* nothing restarts by itself */
    CHECK(ManualModeEnableMUX);
}

/* ---------------- sequence and schedule records (vm_records.c) ---------------- */

static void t_record_checks(void)
{
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMINTERCONNECTIONS, 1), PERR_OK);
    CHECK_EQ(write_i(VMP_FIRSTINLETS, 3), PERR_OK);
    CHECK_EQ(write_i(VMP_FIRSTOUTLETS, 2), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMBEROFMUXVALVES, 4), PERR_OK);
    scans(3);
    char why[96];

    Sequences_t v; memset(&v, 0, sizeof v);
    v.NumberOfPeriods = 2; v.NumberArray[0] = 1; v.NumberArray[1] = 2;
    v.Inlets[0] = 1; v.Inlets[1] = 3; v.Outlets[0] = 1; v.Outlets[1] = 2; v.Timers[0] = v.Timers[1] = 60000;
    strcpy(v.Name[0], "Zero air"); strcpy(v.Name[1], "Span");
    CHECK_EQ(vm_check_record(VMP_SEQUENCE1, &v, why, sizeof why), 0);
    v.Inlets[1] = 4;
    CHECK(vm_check_record(VMP_SEQUENCE1, &v, why, sizeof why) != 0);
    CHECK(strcmp(why, "Period 2: inlet 4 is not fitted (interconnection 1 has 3)") == 0);
    v.Inlets[1] = 3; v.NumberArray[1] = 5;
    CHECK(vm_check_record(VMP_SEQUENCE1, &v, why, sizeof why) != 0);   /* S10: a wrong row number stalls the run */
    v.NumberArray[1] = 2; v.Outlets[5] = 1;
    CHECK(vm_check_record(VMP_SEQUENCE1, &v, why, sizeof why) != 0);   /* rung 1 checks every row */
    v.Outlets[5] = 0; memset(v.Name[0], 'x', 13);
    CHECK(vm_check_record(VMP_SEQUENCE1, &v, why, sizeof why) != 0);   /* unterminated name */
    strcpy(v.Name[0], "Zero air");
    CHECK(vm_check_record(VMP_SEQUENCE2, &v, why, sizeof why) != 0);   /* interconnection 2 has no valves */
    memset(&v, 0, sizeof v);
    CHECK_EQ(vm_check_record(VMP_SEQUENCE2, &v, why, sizeof why), 0);  /* an empty sequence is allowed */

    ScheduleMUX_t m; memset(&m, 0, sizeof m);
    m.NumberOfPeriods = 3; m.NumberArray[0] = 1; m.NumberArray[1] = 2; m.NumberArray[2] = 3;
    m.SingulValves[0] = 0; m.SingulValves[1] = 1; m.SingulValves[2] = 0;   /* inlet 1, 2, 1 (S7, S8 fixed) */
    CHECK_EQ(vm_check_record(VMP_SEQUENCEMUX, &m, why, sizeof why), 0);
    m.SingulValves[2] = 3;                                               /* inlet 4: not shown with 4 valves */
    CHECK(vm_check_record(VMP_SEQUENCEMUX, &m, why, sizeof why) != 0);
    CHECK(strncmp(why, "Channel 3: inlet 4 is not fitted", 32) == 0);
    m.SingulValves[2] = 0; ((uint8_t *)m.FastLoopPump)[0] = 7;
    CHECK(vm_check_record(VMP_SEQUENCEMUX, &m, why, sizeof why) != 0);   /* not a bool */

    Schedule_t c; memset(&c, 0, sizeof c);
    c.From_Hour[2] = 6; c.To_Hour[2] = 6; c.To_Hour[1] = 5;              /* 06:00:00 .. 06:05:00 */
    CHECK_EQ(vm_check_record(VMP_SCHEDULE1, &c, why, sizeof why), 0);
    c.To_Hour[1] = 0;
    CHECK(vm_check_record(VMP_SCHEDULE1, &c, why, sizeof why) != 0);     /* end not after start */
    c.To_Hour[1] = 5; c.From_Hour[2] = 24;
    CHECK(vm_check_record(VMP_SCHEDULEMUX, &c, why, sizeof why) != 0);
    c.From_Hour[2] = 6; c.From_Year = 2027; c.To_Year = 2026;
    CHECK(vm_check_record(VMP_SCHEDULE2, &c, why, sizeof why) != 0);
    CHECK(vm_check_record(9999, &c, why, sizeof why) != 0);

    /* the descriptions cover each record exactly */
    CHECK_EQ(vm_n_records, 6);
    for (size_t r = 0; r < vm_n_records; r++)
        for (unsigned f = 0; f < vm_records[r].n_fields; f++)
            CHECK(vm_records[r].fields[f].offset + vm_records[r].fields[f].size <= vm_records[r].size);
}

static void t_mux_repeated_inlet(void)
{
    /* S7/S8 fixed: channels inlet 1, inlet 2, inlet 1 - inlet 1 opens in channels 1 and 3 */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMBEROFMUXVALVES, 4), PERR_OK);
    const int16_t map[3] = { 5, 6, 7 };
    write_i16_array(VMP_DOSINGULARINLET, map, 3);
    memset(&SequenceMUX, 0, sizeof SequenceMUX);
    SequenceMUX.NumberOfPeriods = 3;
    for (int k = 0; k < 3; k++) { SequenceMUX.NumberArray[k] = (uint8_t)(k + 1); SequenceMUX.DwellTimer[k] = 1000; SequenceMUX.SampleTimer[k] = 1000; }
    SequenceMUX.SingulValves[0] = 0; SequenceMUX.SingulValves[1] = 1; SequenceMUX.SingulValves[2] = 0;
    char why[96]; CHECK_EQ(vm_check_record(VMP_SEQUENCEMUX, &SequenceMUX, why, sizeof why), 0);
    scans(2);
    CHECK_EQ(press(VMC_MUX_SCHEDULE, 1), PERR_OK);
    uint32_t at[3]; scans(100); at[0] = bsp_host.out.do_bits; scans(200); at[1] = bsp_host.out.do_bits; scans(200); at[2] = bsp_host.out.do_bits;
#if VM_FIX_MUX_VALVE_ASSIGN
    CHECK_EQ(at[0], 1u << 4);    /* channel 1: inlet 1 (DO5) */
    CHECK_EQ(at[1], 1u << 5);    /* channel 2: inlet 2 (DO6) */
    CHECK_EQ(at[2], 1u << 4);    /* channel 3: inlet 1 again */
#else
    CHECK_EQ(at[0], 0u);         /* S8: unused rows switch inlet 1 off */
#endif
    press(VMC_MUX_SCHEDULE, 1); scans(3);
}

static void t_vm_v7_long_tube2_sequence(void)
{
    /* V7 fixed: a 10-period sequence on interconnection 2 runs past period 9 and ends */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 20), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMINTERCONNECTIONS, 2), PERR_OK);
    CHECK_EQ(write_i(VMP_FIRSTINLETS, 2), PERR_OK);  CHECK_EQ(write_i(VMP_FIRSTOUTLETS, 2), PERR_OK);
    CHECK_EQ(write_i(VMP_SECONDINLETS, 2), PERR_OK); CHECK_EQ(write_i(VMP_SECONDOUTLETS, 2), PERR_OK);
    scans(3);
    char why[96];
    memset(&Sequence1, 0, sizeof Sequence1); memset(&Sequence2, 0, sizeof Sequence2);
    Sequence1.NumberOfPeriods = 1; Sequence1.NumberArray[0] = 1; Sequence1.Inlets[0] = 1; Sequence1.Outlets[0] = 1; Sequence1.Timers[0] = 1000;
    Sequence2.NumberOfPeriods = 10;
    for (int k = 0; k < 10; k++) { Sequence2.NumberArray[k] = (uint8_t)(k + 1); Sequence2.Inlets[k] = 1; Sequence2.Outlets[k] = 2; Sequence2.Timers[k] = 1000; }
    CHECK_EQ(vm_check_record(VMP_SEQUENCE1, &Sequence1, why, sizeof why), 0);
    CHECK_EQ(vm_check_record(VMP_SEQUENCE2, &Sequence2, why, sizeof why), 0);
    memset(&Schedule1, 0, sizeof Schedule1); memset(&Schedule2, 0, sizeof Schedule2);
    Schedule1.To_Hour[2] = 23; Schedule2.To_Hour[2] = 23;              /* 00:00 .. 23:00 */
    scans(2);
    CHECK_EQ(press(VMC_VM_SCHEDULE, 1), PERR_OK);
    scans(5);
    CHECK(StartInterconnection2Periods);
    bool ended = false;
    for (int t = 0; t < 1300 && !ended; t++) { scans(1); if (!StartInterconnection2Periods) ended = true; }
#if VM_FIX_V7
    CHECK(ended);                                                       /* ~10 s, past period 9 */
#else
    CHECK(!ended);                                                      /* V7: stalls at period 9 */
#endif
    press(VMC_VM_SCHEDULE, 1); scans(3);
}

static void t_vm_schedule_repeat(void)
{
    /* V9: a VM run that ends inside its window starts again (as S3 on the MUX) */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMINTERCONNECTIONS, 1), PERR_OK);
    CHECK_EQ(write_i(VMP_FIRSTINLETS, 3), PERR_OK); CHECK_EQ(write_i(VMP_FIRSTOUTLETS, 2), PERR_OK);
    scans(3);
    const int16_t din[3] = { 0, 1, 2 }, dout[5] = { 0, 0, 0, 3, 4 };
    write_i16_array(VMP_DOINLET, din, 3); write_i16_array(VMP_DOOUTLET, dout, 5);
    memset(&Sequence1, 0, sizeof Sequence1);
    Sequence1.NumberOfPeriods = 2;
    Sequence1.NumberArray[0] = 1; Sequence1.Inlets[0] = 1; Sequence1.Outlets[0] = 1; Sequence1.Timers[0] = 2000;
    Sequence1.NumberArray[1] = 2; Sequence1.Inlets[1] = 3; Sequence1.Outlets[1] = 2; Sequence1.Timers[1] = 2000;
    memset(&Schedule1, 0, sizeof Schedule1); Schedule1.To_Hour[2] = 23;
    scans(2);
    CHECK_EQ(press(VMC_VM_SCHEDULE, 1), PERR_OK);
    uint32_t seen[10]; int when[10], n = 0; uint32_t last = 0xFFFFFFFFu;
    for (int t = 0; t < 1000; t++) {
        scans(1);
        if (bsp_host.out.do_bits != last && n < 10) { last = bsp_host.out.do_bits; seen[n] = last; when[n++] = t * 10; }
    }
    CHECK(n >= 3);
    CHECK_EQ(seen[0], (1u << 0) | (1u << 3));   /* inlet 1 + outlet 1 (DO1, DO4) */
    CHECK_EQ(seen[1], (1u << 2) | (1u << 4));   /* inlet 3 + outlet 2 (DO3, DO5) after 2 s */
#if VM_FIX_VM_RESTART
    CHECK(n >= 5);                              /* F7 fixed: the run repeats inside the window (V9) */
    CHECK_EQ(seen[3], (1u << 0) | (1u << 3));
    CHECK(when[3] - when[1] <= 2100);
#else
    CHECK_EQ(n, 3);                             /* the original stands still after the first run */
    CHECK_EQ(seen[2], 0u);
    CHECK(StartInterconnection1Periods);        /* ... with its run flag stuck on */
#endif
    press(VMC_VM_SCHEDULE, 1); scans(3);
}

static void t_mux_digital_stop(void)
{
    /* MUX DI function "Stop Seq." (24 without the add-on DI module, 44 with it, 43b) on DI5 stops the loop;
     * releasing it starts the loop again from channel 1 */
    memset(bsp_host_fram, 0, sizeof bsp_host_fram);
    power_up();
    scans(2);
    CHECK_EQ(write_i(VMP_MAXNUMBEROFVALVES, 10), PERR_OK);
    CHECK_EQ(write_i(VMP_NUMBEROFMUXVALVES, 4), PERR_OK);
    const int16_t map[3] = { 5, 6, 7 };
    write_i16_array(VMP_DOSINGULARINLET, map, 3);
    scans(2);
    DI_FunctionMUX[4] = AdditionalDiscreteModule ? 44 : 24;
    memset(&SequenceMUX, 0, sizeof SequenceMUX);
    SequenceMUX.NumberOfPeriods = 2;
    for (int k = 0; k < 2; k++) { SequenceMUX.NumberArray[k] = (uint8_t)(k + 1); SequenceMUX.DwellTimer[k] = 1000; SequenceMUX.SampleTimer[k] = 2000; }
    SequenceMUX.SingulValves[0] = 0; SequenceMUX.SingulValves[1] = 1;
    scans(2);
    CHECK_EQ(press(VMC_MUX_SCHEDULE, 1), PERR_OK);
    scans(400);                                          /* 4 s: channel 2 */
    CHECK_EQ(bsp_host.out.do_bits, 1u << 5);
    bsp_host.in.di = 1u << 4; scans(5);                  /* DI5 closed */
    CHECK(StopSeqBit); CHECK(!StartPeriodsMUX);
    CHECK_EQ(bsp_host.out.do_bits, 0u);
    CHECK(alarm_active(VM_AL_DIGITAL_STOP));
    scans(300); CHECK_EQ(bsp_host.out.do_bits, 0u);      /* held while the stop is on */
    bsp_host.in.di = 0; scans(5);                        /* released: again from channel 1 */
    CHECK(StartPeriodsMUX);
    CHECK_EQ(bsp_host.out.do_bits, 1u << 4);
    press(VMC_MUX_SCHEDULE, 1); scans(3);
    DI_FunctionMUX[4] = 0;
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
    RUN(t_alarms);
    RUN(t_modbus);
    RUN(t_q1_link_loss);
    RUN(t_q5_q6_retained);
    RUN(t_mux_manual_run);
    RUN(t_vm_modbus_run_and_link_loss);
    RUN(t_mux_schedule_run);
    RUN(t_mux_master_enable);
    RUN(t_record_checks);
    RUN(t_mux_repeated_inlet);
    RUN(t_vm_v7_long_tube2_sequence);
    RUN(t_vm_schedule_repeat);
    RUN(t_mux_digital_stop);
    printf("\nvm_tests: %d checks passed, %d failed\n", tst_pass, tst_fail);
    return tst_fail ? 1 : 0;
}
