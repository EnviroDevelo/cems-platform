/* task_monitor.c - see task_monitor.h
 *
 * Each entry has a reference time (ref_ms) and an allowed silence (allow_ms):
 *   before the first check-in: ref = boot (or re-enable) time,
 *                              allow = max(startup grace, max_silence) at boot,
 *                                      max_silence after a re-enable;
 *   after a check-in:          ref = last check-in, allow = max_silence. */
#include <string.h>
#include "task_monitor.h"

void tm_init(task_monitor_t *m, uint32_t now_ms, uint32_t startup_grace_ms) {
    memset(m, 0, sizeof *m);
    m->boot_ms = now_ms;
    m->grace_ms = startup_grace_ms;
}

int tm_register(task_monitor_t *m, const char *name, uint32_t max_silence_ms) {
    if (m->count >= TM_MAX_TASKS) return -1;
    tm_entry_t *e = &m->e[m->count];
    e->name = name;
    e->max_silence_ms = max_silence_ms;
    e->last_ms = m->boot_ms;
    e->first_allow_ms = m->grace_ms > max_silence_ms ? m->grace_ms : max_silence_ms;
    e->started = false;
    e->enabled = true;
    return m->count++;
}

void tm_checkin(task_monitor_t *m, int id, uint32_t now_ms) {
    if (id < 0 || id >= m->count) return;
    m->e[id].last_ms = now_ms;     /* aligned word: atomic on Cortex-M */
    m->e[id].started = true;
}

void tm_enable(task_monitor_t *m, int id, bool en, uint32_t now_ms) {
    if (id < 0 || id >= m->count) return;
    tm_entry_t *e = &m->e[id];
    if (en && !e->enabled) {           /* restart the window from now */
        e->last_ms = now_ms;
        e->first_allow_ms = e->max_silence_ms;
        e->started = false;
    }
    e->enabled = en;
}

int tm_check(const task_monitor_t *m, uint32_t now_ms) {
    for (int i = 0; i < m->count; ++i) {
        const tm_entry_t *e = &m->e[i];
        if (!e->enabled) continue;
        uint32_t allow = e->started ? e->max_silence_ms : e->first_allow_ms;
        if ((uint32_t)(now_ms - e->last_ms) > allow) return i;
    }
    return TM_ALL_OK;
}

const char *tm_name(const task_monitor_t *m, int id) {
    return (id >= 0 && id < m->count && m->e[id].name) ? m->e[id].name : "?";
}
