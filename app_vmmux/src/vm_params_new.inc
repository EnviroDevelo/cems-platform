/* vm_params_new.inc - parameters the original program did not have. Included at the end
 * of the generated table in vm_params.c. IDs 2900+ are registered in tools/param_ids.csv.
 *
 * valve_fb_settle_s: valve-feedback alarms wait this long before raising (D19: default
 *   5 s; finding N11 / 43b note 4: the original had no settling time, so every valve
 *   change could raise and email a feedback alarm). Used as delay_param of alarms 130..149.
 * vm_link_close_min (Q1, Gary 20 Oct): a Valve Manager run started over Modbus closes its
 *   valves when the remote link has been lost this many minutes (default 30; 0 = never).
 *   The Multiplexor carries on: it is the automated loop. See vm_app.c.
 * rtd1/2_alarm_delay_s (Q5): on and off delay of the temperature range alarms. Default 10 s,
 *   the delay the old temperature emails used (49, Email rung 16) and the analog alarms use.
 * mux_enable (Gary 20 Oct): master enable of the whole Multiplexor feature (vm_mux.c). Default on.
 * rtd1/2_alarm_hyst (Q5): hysteresis of the temperature range alarms, in the units of the
 *   limits (0.1 degree, as TemperatureSensor). Default 0 = none, as the original. */
PARAM_UINT(VM_PARAM_FB_SETTLE_S, "valve_fb_settle_s", vm_fb_settle_s, CT_U16, 0u, 600u, 5u, PLAT_LVL_ADMIN, PF_PERSIST, "s"),
PARAM_UINT(VM_PARAM_LINK_CLOSE_MIN, "vm_link_close_min", vm_link_close_min, CT_U16, 0u, 65535u, 30u, PLAT_LVL_ADMIN, PF_PERSIST, "min"),
PARAM_UINT(VM_PARAM_RTD1_DELAY_S, "rtd1_alarm_delay_s", vm_rtd_delay_s[0], CT_U16, 0u, 65535u, 10u, PLAT_LVL_ADMIN, PF_PERSIST, "s"),
PARAM_UINT(VM_PARAM_RTD2_DELAY_S, "rtd2_alarm_delay_s", vm_rtd_delay_s[1], CT_U16, 0u, 65535u, 10u, PLAT_LVL_ADMIN, PF_PERSIST, "s"),
PARAM_REAL(VM_PARAM_RTD1_HYST, "rtd1_alarm_hyst", vm_rtd_hyst[0], 0.0f, 3.4e38f, 0.0f, NULL, PLAT_LVL_ADMIN, PF_PERSIST),
PARAM_REAL(VM_PARAM_RTD2_HYST, "rtd2_alarm_hyst", vm_rtd_hyst[1], 0.0f, 3.4e38f, 0.0f, NULL, PLAT_LVL_ADMIN, PF_PERSIST),
PARAM_BIT_D(VM_PARAM_MUX_ENABLE, "mux_enable", vm_mux_enable, true, PLAT_LVL_ADMIN, PF_PERSIST),
