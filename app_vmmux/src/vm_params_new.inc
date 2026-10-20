/* vm_params_new.inc - parameters the original program did not have. Included at the end
 * of the generated table in vm_params.c. IDs 2900+ are registered in tools/param_ids.csv.
 *
 * valve_fb_settle_s: valve-feedback alarms wait this long before raising (D19: default
 *   5 s; finding N11 / 43b note 4: the original had no settling time, so every valve
 *   change could raise and email a feedback alarm). Used as delay_param of alarms 130..149. */
PARAM_UINT(VM_PARAM_FB_SETTLE_S, "valve_fb_settle_s", vm_fb_settle_s, CT_U16, 0u, 600u, 5u, PLAT_LVL_ADMIN, PF_PERSIST, "s"),
