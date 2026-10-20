/* t4_support.c - what the Stage 4 tests used to get from the faithful files before
 * integration: the diagnostics counters, the system tags (in the platform they are
 * scan_engine.c's) and, for 4C, the TwoORFb_22 stub. Selected per test by -D flags. */
#include <stdint.h>
#include <stdbool.h>
#include "ul_system.h"
#ifndef T4_HAVE_TAGS            /* vm_tags.c not linked: define the counters here */
#define W __attribute__((weak))   /* a test that defines its own copy wins */
W int g_oob; W int g_div0; W int g_hang; W int g_out_of_range_writes; W int g_twoorfb22_calls;
#endif
#ifdef T4_SYS_TAGS
uint32_t g_dt_ms = 10;
bool Ladder_Initial_Cycle;
rtc_date_t RTC_Date = { .Day_Of_Week = 1, .Day_In_Month = 18, .Month = 10, .Year = 2026 };
rtc_time_t RTC_Time = { .Seconds = 0, .Minutes = 5, .Hours = 9 };
#endif
#ifdef T4_STUB_TWOORFB22
void TwoORFb_22(void);
void TwoORFb_22(void) { g_twoorfb22_calls++; }
#endif
