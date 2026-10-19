/* ul_system.h - UniLogic system tags and shared types, owned by the core platform.
 *
 * The Stage 4 translations of the old VM/MUX program read these names directly.
 * This header is the ONE definition of them: application chunks must include it
 * instead of declaring their own copies (see docs/PLATFORM.md, "Integration rules").
 *
 * Who writes what:
 *   g_dt_ms, Ladder_Initial_Cycle, RTC_Date, RTC_Time  - written by the scan engine
 *                                                         (scan_engine.c) before each
 *                                                         call to the application scan.
 *   Everything else in the application                  - written by the application.
 *
 * Layouts match 11_UniLogic_Semantics.md section 4 and Stage 4 headers 4E.h / 4H.h.
 * NOTE: 4F2.h declares rtc_date_t with a different member order; that copy must be
 * removed in favour of this one (members are accessed by name, so no code change).
 */
#ifndef UL_SYSTEM_H
#define UL_SYSTEM_H
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RTC_Date: Day_Of_Week 1 = Sunday ... 7 = Saturday (11 section 4, SEC).
 * Year is the full year (e.g. 2026). Month 1..12, Day_In_Month 1..31. */
typedef struct {
    uint8_t  Day_Of_Week;
    uint8_t  Day_In_Month;
    uint8_t  Month;
    uint16_t Year;
} rtc_date_t;

/* RTC_Time: 24-hour clock. */
typedef struct {
    uint8_t Seconds;
    uint8_t Minutes;
    uint8_t Hours;
} rtc_time_t;

/* UniLogic Timer struct (11 section 4): Preset and Current in milliseconds,
 * Current counts DOWN from Preset to 0. Timer behaviour (TON/TP/TA) is
 * modelled in the application layer; the platform only supplies g_dt_ms. */
typedef struct {
    uint32_t Preset;
    uint32_t Current;
    bool     Out;
    bool     On;
} ul_timer_t;

/* Milliseconds elapsed since the previous scan started. 0 on the first scan.
 * Measured, not assumed: a late scan reports the real elapsed time, so
 * timers never lose time (they may step by more than one period). */
extern uint32_t   g_dt_ms;

/* General.Ladder_Initial_Cycle: true during the first scan after start-up only (E10). */
extern bool       Ladder_Initial_Cycle;

/* Clock snapshot taken once at the start of each scan, so every rung in one
 * scan sees the same time (the original reads the clock per rung; one
 * snapshot per scan is repeatable and differs by at most one scan period). */
extern rtc_date_t RTC_Date;
extern rtc_time_t RTC_Time;

#ifdef __cplusplus
}
#endif
#endif
