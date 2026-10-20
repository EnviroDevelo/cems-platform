/* vm_types.h - the ONE definition of the VM/MUX project types (21_Types.h.md).
 *
 * The Stage 4 chunk headers each carried their own copy of these typedefs (and of
 * ul_timer_t / rtc_date_t / rtc_time_t). Linked together, differing copies of one
 * global's type are undefined behaviour (PLATFORM.md section 6, rules 1-3), so every
 * chunk now includes this header instead. System tags and ul_timer_t come from the
 * platform's ul_system.h.
 *
 * Layouts:
 *   Schedule_t           UniLogic system type Schedule (4D.h, project DB member order).
 *                        From_Hour / To_Hour = [Seconds, Minutes, Hours].
 *   Sequences_t          project type Sequences (VM). Name1..Name20 are 20 consecutive
 *                        STRING(12) members; 4F2 indexed them as Name[k]. Both spellings
 *                        are kept through an anonymous union over the same bytes.
 *   ScheduleMUX_t        project type ScheduleMUX (MUX sequence).
 *   LoggingTableStruct_t project type LoggingTableStruct (data-table row).
 */
#ifndef VM_TYPES_H
#define VM_TYPES_H
#include <stdint.h>
#include <stdbool.h>
#include "ul_system.h"
#include "vm_config.h"

typedef struct {
    uint16_t From_Year, To_Year;
    bool     Month[12];
    bool     Day_Of_Month[31];
    uint8_t  Spare1;
    bool     Day_Of_Week[7];
    uint8_t  Spare2;
    uint8_t  From_Hour[3];             /* [Seconds, Minutes, Hours] */
    uint8_t  Spare3;
    uint8_t  To_Hour[3];
    uint8_t  Spare4;
} Schedule_t;

typedef struct {
    uint8_t  NumberArray[20];
    union {
        struct {
            char Name1[13], Name2[13], Name3[13], Name4[13], Name5[13], Name6[13], Name7[13],
                 Name8[13], Name9[13], Name10[13], Name11[13], Name12[13], Name13[13], Name14[13],
                 Name15[13], Name16[13], Name17[13], Name18[13], Name19[13], Name20[13];
        };
        char Name[20][13];             /* Name[k-1] is Name<k> (4F2 spelling) */
    };
    uint32_t Timers[20];
    uint8_t  Inlets[20];
    uint8_t  Outlets[20];
    uint8_t  NumberOfPeriods;
} Sequences_t;

typedef struct {
    uint8_t  NumberArray[20];
    uint8_t  SingulValves[20];
    uint32_t DwellTimer[20];
    uint32_t SampleTimer[20];
    bool     FastLoopPump[20];
    uint32_t FastLoopPumpTimer[20];
    uint8_t  NumberOfPeriods;
} ScheduleMUX_t;

typedef struct {
    char DateTime[18 + 1];
    char OpenedValves[220 + 1];
    char Col3[18 + 1],  Col4[18 + 1],  Col5[18 + 1],  Col6[18 + 1],  Col7[18 + 1];
    char Col8[18 + 1],  Col9[18 + 1],  Col10[18 + 1], Col11[18 + 1], Col12[18 + 1];
    char Col13[18 + 1], Col14[18 + 1], Col15[18 + 1], Col16[18 + 1], Col17[18 + 1];
    char Col18[18 + 1], Col19[18 + 1], Col20[18 + 1], Col21[18 + 1], Col22[18 + 1];
} LoggingTableStruct_t;

/* Diagnostics kept from the Stage 4 test aids. On the target they count events the
 * original would have handled silently (A4.1 / A4.8 out-of-bounds index, A4.16 MOD by
 * zero, A4.9 endless FOR); they are shown on the diagnostics page, never acted on. */
extern int g_oob;
extern int g_div0;
extern int g_hang;

#endif
