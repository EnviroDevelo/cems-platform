/* task_monitor.h - task liveness for the watchdog supervisor.
 *
 * Every platform task registers with a maximum silence and calls tm_checkin()
 * from its main loop. The supervisor calls tm_check() every
 * PLAT_SUPERVISOR_PERIOD_MS and kicks the IWDG only if every enabled task has
 * checked in recently enough. One stuck task (or a starved low-priority task)
 * therefore resets the unit, and the supervisor records which task it was in
 * the diagnostic record (plat_diag.h) before the reset.
 *
 * Pure C, time passed in, so it is tested on the host. */
#ifndef TASK_MONITOR_H
#define TASK_MONITOR_H
#include <stdint.h>
#include <stdbool.h>

#define TM_MAX_TASKS 12
#define TM_ALL_OK    (-1)

typedef struct {
    const char *name;
    uint32_t max_silence_ms;
    volatile uint32_t last_ms; /* last check-in, or boot / re-enable time before the first */
    uint32_t first_allow_ms;   /* silence allowed before the first check-in */
    volatile bool started;     /* first check-in seen */
    bool enabled;
} tm_entry_t;

typedef struct {
    tm_entry_t e[TM_MAX_TASKS];
    int        count;
    uint32_t   boot_ms;
    uint32_t   grace_ms;
} task_monitor_t;

void tm_init(task_monitor_t *m, uint32_t now_ms, uint32_t startup_grace_ms);
int  tm_register(task_monitor_t *m, const char *name, uint32_t max_silence_ms); /* id, or -1 if full */
void tm_checkin(task_monitor_t *m, int id, uint32_t now_ms);
void tm_enable(task_monitor_t *m, int id, bool en, uint32_t now_ms); /* e.g. pause while a long flash erase blocks a task */
int  tm_check(const task_monitor_t *m, uint32_t now_ms); /* TM_ALL_OK or id of first silent task */
const char *tm_name(const task_monitor_t *m, int id);

#endif
