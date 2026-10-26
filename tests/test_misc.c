/* test_misc.c - command queue, seqlock, task monitor, diagnostic record. */
#include <string.h>
#include "tst.h"
#include "plat_cmdq.h"
#include "plat_seqlock.h"
#include "task_monitor.h"
#include "plat_diag.h"

/* ---------------- command queue ---------------- */
static void t_cmdq_fifo_full_wrap(void) {
    plat_cmdq_t q; plat_cmdq_init(&q);
    q.head = q.tail = 0xFFFFFFF0u;                 /* counters wrap during the test */
    plat_cmd_t c = { 0 }, o;
    for (int k = 0; k < (int)PLAT_CMDQ_DEPTH; ++k) { c.id = (uint16_t)k; CHECK(plat_cmdq_post(&q, &c)); }
    CHECK_EQ(plat_cmdq_count(&q), PLAT_CMDQ_DEPTH);
    c.id = 999; CHECK(!plat_cmdq_post(&q, &c));    /* full: refused and counted */
    CHECK_EQ(q.dropped, 1);
    int ok = 1;
    for (int k = 0; k < (int)PLAT_CMDQ_DEPTH; ++k) { if (!plat_cmdq_pop(&q, &o) || o.id != k) ok = 0; }
    CHECK(ok);
    CHECK(!plat_cmdq_pop(&q, &o));
    CHECK_EQ(plat_cmdq_count(&q), 0);
}

static void t_cmdq_payload(void) {
    plat_cmdq_t q; plat_cmdq_init(&q);
    plat_cmd_t c = { .id = 42, .src = PLAT_SRC_MODBUS, .kind = PLAT_VAL_F32, .v.f = 3.25f, .token = 77 }, o;
    plat_cmdq_post(&q, &c); plat_cmdq_pop(&q, &o);
    CHECK_EQ(o.id, 42); CHECK_EQ(o.src, PLAT_SRC_MODBUS); CHECK_EQ(o.token, 77);
    CHECK(o.v.f == 3.25f);
}

void test_cmdq_all(void) { RUN(t_cmdq_fifo_full_wrap); RUN(t_cmdq_payload); }

/* ---------------- seqlock ---------------- */
void (*plat_seq_test_hook)(void);
static plat_seqlock_t L;
static struct { int a, b; } shared, copy;
static int hook_writes;
static void writer_interrupts(void) {             /* the scan "preempts" the reader once */
    if (hook_writes-- > 0) { plat_seq_write_begin(&L); shared.a++; shared.b++; plat_seq_write_end(&L); }
}

static void t_seq_consistent_retry(void) {
    L.seq = 0; shared.a = shared.b = 5;
    hook_writes = 1; plat_seq_test_hook = writer_interrupts;
    CHECK(plat_seq_read(&L, &copy, &shared, sizeof shared, 4));
    CHECK_EQ(copy.a, 6); CHECK_EQ(copy.b, 6);      /* second attempt saw the new values */
    plat_seq_test_hook = NULL;
}

static void t_seq_bounded(void) {
    L.seq = 1;                                     /* writer "stuck" mid-write (higher-prio reader case) */
    CHECK(!plat_seq_read(&L, &copy, &shared, sizeof shared, 3));
    L.seq = 2;
    hook_writes = 100; plat_seq_test_hook = writer_interrupts;   /* changes every attempt */
    CHECK(!plat_seq_read(&L, &copy, &shared, sizeof shared, 3));
    plat_seq_test_hook = NULL;
}

void test_seqlock_all(void) { RUN(t_seq_consistent_retry); RUN(t_seq_bounded); }

/* ---------------- task monitor ---------------- */
static void t_tm_basic(void) {
    task_monitor_t m; tm_init(&m, 1000, 5000);
    int a = tm_register(&m, "a", 100), b = tm_register(&m, "b", 2000);
    CHECK_EQ(a, 0); CHECK_EQ(b, 1);
    CHECK_EQ(tm_check(&m, 1000), TM_ALL_OK);
    CHECK_EQ(tm_check(&m, 6000), TM_ALL_OK);       /* inside startup grace, nobody checked in */
    CHECK_EQ(tm_check(&m, 6001), a);               /* grace over, a never started */
    tm_checkin(&m, a, 6001);
    CHECK_EQ(tm_check(&m, 6001), b);
    tm_checkin(&m, b, 6001);
    CHECK_EQ(tm_check(&m, 6101), TM_ALL_OK);       /* exactly at the limit is fine */
    CHECK_EQ(tm_check(&m, 6102), a);               /* a silent > 100 ms */
    CHECK_EQ(strcmp(tm_name(&m, a), "a"), 0);
}

static void t_tm_wrap(void) {
    task_monitor_t m; tm_init(&m, 0xFFFFFF00u, 0);
    int a = tm_register(&m, "a", 100);
    tm_checkin(&m, a, 0xFFFFFFF0u);
    CHECK_EQ(tm_check(&m, 0x00000050u), TM_ALL_OK);  /* 96 ms later, across the wrap */
    CHECK_EQ(tm_check(&m, 0x00000060u), a);          /* 112 ms */
}

static void t_tm_disable_reenable(void) {
    task_monitor_t m; tm_init(&m, 0, 1000);
    int a = tm_register(&m, "a", 100);
    tm_checkin(&m, a, 10);
    tm_enable(&m, a, false, 20);
    CHECK_EQ(tm_check(&m, 50000), TM_ALL_OK);        /* paused: ignored */
    tm_enable(&m, a, true, 50000);
    CHECK_EQ(tm_check(&m, 50100), TM_ALL_OK);        /* new window from re-enable */
    CHECK_EQ(tm_check(&m, 50101), a);
    tm_checkin(&m, a, 50101);
    CHECK_EQ(tm_check(&m, 50150), TM_ALL_OK);
}

static void t_tm_full(void) {
    task_monitor_t m; tm_init(&m, 0, 0);
    for (int k = 0; k < TM_MAX_TASKS; ++k) CHECK(tm_register(&m, "x", 10) == k);
    CHECK_EQ(tm_register(&m, "y", 10), -1);
    tm_checkin(&m, 99, 0);                           /* bad ids ignored, no crash */
    tm_checkin(&m, -1, 0);
}

void test_monitor_all(void) { RUN(t_tm_basic); RUN(t_tm_wrap); RUN(t_tm_disable_reenable); RUN(t_tm_full); }

/* ---------------- diagnostic record ---------------- */
static void t_diag(void) {
    memset(&plat_diag, 0xA5, sizeof plat_diag);      /* random RAM after power-up */
    plat_diag_boot(BSP_RESET_POWER_ON);
    CHECK(plat_diag_valid());
    CHECK_EQ(plat_diag.boots, 1); CHECK_EQ(plat_diag.fault, PLAT_FAULT_NONE); CHECK_EQ(plat_diag.fault_task, -1);

    plat_diag_fault(PLAT_FAULT_TASK_SILENT, 3, "modbus", 0, 123456);
    plat_diag_boot(BSP_RESET_IWDG);                  /* record survives a warm reset */
    CHECK(plat_diag_valid());
    CHECK_EQ(plat_diag.boots, 2); CHECK_EQ(plat_diag.wdg_resets, 1);
    CHECK_EQ(plat_diag.fault, PLAT_FAULT_TASK_SILENT); CHECK_EQ(plat_diag.fault_task, 3);
    CHECK_EQ(strcmp(plat_diag.fault_name, "modbus"), 0);
    CHECK_EQ(plat_diag.last_reset_cause, BSP_RESET_IWDG);

    plat_diag.fault_task = 9;                        /* corrupted word -> invalid -> cleared at boot */
    CHECK(!plat_diag_valid());
    plat_diag_boot(BSP_RESET_SOFTWARE);
    CHECK_EQ(plat_diag.boots, 1); CHECK_EQ(plat_diag.fault, PLAT_FAULT_NONE);

    plat_diag_fault(PLAT_FAULT_ASSERT, -1, "a_very_long_file_name_here.c", 77, 1);
    CHECK_EQ(strlen(plat_diag.fault_name), sizeof plat_diag.fault_name - 1);   /* truncated, terminated */
    CHECK_EQ(plat_diag.fault_arg, 77);

    /* CPU faults (D4): kind from CFSR/HFSR, PC in the name, fault address only when valid */
    plat_diag_cpu_fault(0x0801A2C4u, 0x08004001u, 0x00008200u, 0x40000000u, 0x11111111u, 0x20012345u, NULL, 99);
    CHECK(plat_diag_valid());
    CHECK_EQ(plat_diag.fault, PLAT_FAULT_HARDFAULT); CHECK_EQ(plat_diag.fault_task, -1);
    CHECK_EQ(strcmp(plat_diag.fault_name, "bus @0801A2C4"), 0);
    CHECK_EQ(plat_diag.fault_arg, 0x0801A2C4u); CHECK_EQ(plat_diag.fault_lr, 0x08004001u);
    CHECK_EQ(plat_diag.fault_cfsr, 0x00008200u); CHECK_EQ(plat_diag.fault_hfsr, 0x40000000u);
    CHECK_EQ(plat_diag.fault_addr, 0x20012345u);               /* BFARVALID */
    plat_diag_cpu_fault(0x08000010u, 0, 0x00000082u, 0x40000000u, 0x00000004u, 0, NULL, 1);
    CHECK_EQ(strcmp(plat_diag.fault_name, "mem @08000010"), 0); CHECK_EQ(plat_diag.fault_addr, 4);   /* MMARVALID */
    plat_diag_cpu_fault(0x080ABCDEu, 0, 0x02000000u, 0x40000000u, 7, 7, "scan", 1);
    CHECK_EQ(strcmp(plat_diag.fault_name, "usage @080ABCDE"), 0); CHECK_EQ(plat_diag.fault_addr, 0);  /* divide by zero: no address */
    plat_diag_cpu_fault(0, 0, 0x00100000u, 0x40000000u, 0, 0, NULL, 1);
    CHECK_EQ(strcmp(plat_diag.fault_name, "stack @00000000"), 0);   /* longest name still fits */
    plat_diag_cpu_fault(0, 0, 0x00100000u, 0x40000000u, 0, 0, "scan", 1);
    CHECK_EQ(strcmp(plat_diag.fault_name, "stk scan"), 0);          /* stack limit: the task, not a PC */
    plat_diag_cpu_fault(0, 0, 0x00100000u, 0x40000000u, 0, 0, "a_long_task_name", 1);
    CHECK_EQ(strcmp(plat_diag.fault_name, "stk a_long_task"), 0);   /* cut to fit */
    plat_diag_cpu_fault(0xFFFFFFFEu, 0, 0, 0x00000002u, 0, 0, NULL, 1);
    CHECK_EQ(strcmp(plat_diag.fault_name, "vect @FFFFFFFE"), 0);
    plat_diag_cpu_fault(0x08001234u, 0, 0, 0x40000000u, 0, 0, "http", 1);
    CHECK_EQ(strcmp(plat_diag.fault_name, "hard @08001234"), 0);
    plat_diag_boot(BSP_RESET_SOFTWARE);                         /* NVIC_SystemReset after the record */
    CHECK(plat_diag_valid()); CHECK_EQ(plat_diag.fault, PLAT_FAULT_HARDFAULT);
    CHECK_EQ(strcmp(plat_diag.fault_name, "hard @08001234"), 0);
    plat_diag_fault(PLAT_FAULT_ASSERT, -1, "x.c", 1, 2);           /* a later fault clears the CPU registers */
    CHECK_EQ(plat_diag.fault_lr, 0); CHECK_EQ(plat_diag.fault_cfsr, 0); CHECK_EQ(plat_diag.fault_addr, 0);
}

void test_diag_all(void) { RUN(t_diag); }
