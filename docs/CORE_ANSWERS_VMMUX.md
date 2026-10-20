# Core platform answers to app_vmmux INTEGRATION.md (R1-R5) and SCREENS.md

20 Oct 2026. Core platform commit `ef58884` or later (branch `main`). Merge `main` into
`app/vmmux`; nothing under `app_vmmux/` was changed by the core thread. Your 12 Stage 4
suites and 270 application checks pass on the merged tree.

## How a whole sequence is passed (R2, SCREENS.md 2.7)

**One record parameter per sequence or schedule, holding the whole C struct.**

```c
static int vm_check_sequence(uint16_t id, const void *img, char *why, size_t cap);   /* 0 = accept */
PARAM_RECORD(VMP_SEQUENCE1,   "Sequence1",   Sequence1,   vm_check_sequence, PLAT_LVL_OPERATOR, PF_PERSIST),
PARAM_RECORD(VMP_SEQUENCE2,   "Sequence2",   Sequence2,   vm_check_sequence, PLAT_LVL_OPERATOR, PF_PERSIST),
PARAM_RECORD(VMP_SEQUENCEMUX, "SequenceMUX", SequenceMUX, vm_check_mux,      PLAT_LVL_OPERATOR, PF_PERSIST),
PARAM_RECORD(VMP_SCHEDULE1,   "Schedule1",   Schedule1,   vm_check_schedule, PLAT_LVL_OPERATOR, PF_PERSIST),
/* ... Schedule2, ScheduleMUX */
```

- **Replacing the retained blocks.** These replace the `RETAIN()` blocks of the same tags.
  - Keep the IDs, so nothing in `param_ids.csv` is renumbered.
  - Remove the six entries from `retain[]`.
  - A record is a table entry, so it must not also be a retained block.
- **How a write is applied.**
  - The only way to write a record is whole: `param_record_post()`, the editors, or `POST /api/record`.
  - The scan applies it at its next command point, all in one scan: the access and mode checks run again, then your check sees the complete new image, then the tag is replaced.
  - If the check returns non-zero, nothing changes and the operator sees your `why`. For example: `"Period 4: inlet 7 is not fitted"`.
  - Chunked block writes and `/api/write` are refused for records.
- **Records and Modbus.** Records are never on Modbus. If the master needs to read or write sequences, tell the core thread.
- **Reading.** The logic reads the tag as before. Other tasks use `param_read_blob()`.
- **What can go.** `ScheduleHMI`, `HMISequences`, `HMISequenceMUX`, the 7-row scroll bits and the Save bits are no longer needed for editing. Keep them only if logic reads them outside the old editor.
- **Mode.** Add `PF_ANY_MODE` to a record if the sequence may be edited while the PLC has REMOTE control. Without it, the same LOCAL/REMOTE rule as other settings applies.

Describe each record's fields once, so the platform can draw the editor on the
touchscreen and the web page. Example for `Sequences_t` (table: one row per period):

```c
static const ui_field_t seq_fields[] = {
    { .label = "Name",   .kind = RF_TEXT,    .off = offsetof(Sequences_t, Name),   .stride = 13, .size = 13 },
    { .label = "Time",   .kind = RF_MS_AS_S, .off = offsetof(Sequences_t, Timers), .stride = 4, .min = 0, .max = 86400000 },
    { .label = "Inlet",  .kind = RF_UINT, .ctype = CT_U8, .off = offsetof(Sequences_t, Inlets),  .stride = 1, .min = 0, .max = 19, .max_param = VMP_FIRSTINLETS },
    { .label = "Outlet", .kind = RF_UINT, .ctype = CT_U8, .off = offsetof(Sequences_t, Outlets), .stride = 1, .min = 0, .max = 19, .max_param = VMP_FIRSTOUTLETS },
};
static const ui_record_t vm_records[] = {
    { .param_id = VMP_SEQUENCE1, .title = "Sequence 1", .rows = 20,
      .rows_off = offsetof(Sequences_t, NumberOfPeriods), .rows_ctype = CT_U8, .fields = seq_fields, .n_fields = 4 },
    /* Schedule_t as a form: From_Year/To_Year (RF_UINT, CT_U16), Month (RF_BOOL, count 12, items = month names),
       Day_Of_Month (RF_BOOL, count 31), Day_Of_Week (RF_BOOL, count 7, items Sun..Sat), From_Hour/To_Hour (RF_HMS) */
};
```

`NumberArray` is not shown in this example. Your check can renumber it (1..N) before
accepting, or you can show it as a field. The platform rejects a description at start-up
if it names a missing ID or if a field runs past the end of its struct (`ui_app_check`).

## The rest of the screen API (R2)

Everything in SCREENS.md maps onto `ui_widget_t` (`platform/include/ui_app.h`):

| SCREENS.md | Widget |
|---|---|
| Mode buttons 5000-5002 with states 5100-5102 and enables 5103-5105 | `UW_BUTTON` with `cmd_id`, `state_id` and `enable_id` |
| Manual-mode dialog 5106, Yes 5003 / No 5004 | `UW_DIALOG` with `show_id` 5106, `cmd_id` 5003 and `cmd2_id` 5004 |
| Start button, visible 5107, command 5005 | `UW_BUTTON` with `show_id` 5107 |
| Inlets 1..FirstInlets, touch 5006, touchable 5110[i], names 1069+ | `UW_GRID` with `count_id` = VMP_FIRSTINLETS, `cmd_id` 5006, `enable_id` 5110 (array), `names_id` 1069, `state_id` (valve state array, once exposed) |
| Second tube only when NumInterconnections = 2 | any widget: `show_id` = VMP_NUMINTERCONNECTIONS, `show_min` = 2 |
| MUX inlet list: visible 5131[i], on 5132[i], toggle 5016, enabled while 5128 | `UW_GRID` with `show_id` 5131 (array: visible items), `state_id` 5132, `enable_id` 5128 (scalar), `cmd_id` 5016, `names_id` 1122 |
| Remaining time 4045, readings 4145[0..3] | `UW_VALUE` (with `index` for an array element) |
| "Sequences absent" dialog 5108, OK 5008 | `UW_DIALOG` with `cmd2_id` 0, which shows OK only |
| Settings screens 2.2-2.6 | `UW_SECTION` and `UW_VALUE` (tap to change, with the usual level and mode checks) |
| Editors 2.7 | `UW_RECORD` with the record's ID |

Put them in `plat_app_t.ui = &vm_ui` (`ui_app_t`: screens, records). The screens come
first on the touchscreen as tabs, with the platform's Status screen beside them. The web
page shows the same screens under its own heading.

- A button writes `cmd_value` (default 1). Use `UW_WRITE_ZERO` to write 0.
- Commands with `PF_CONFIRM` ask "are you sure?" first.
- A command your interlock refuses at apply time changes nothing, as before.

A complete worked example is the demo app (`app_demo/app_demo.c`). It has a record and a
screen, and the end-to-end web test drives them. A VM-shaped sequence and schedule are in
`tests/test_ui_app.c`.

## R1: choosing the application without the define

Every application defines `app_select()` (declared in `plat_tasks.h`). Add these two files:

`app_vmmux/app_vars.mk`
```make
APP_SRC  = $(wildcard app_vmmux/faithful/*_faithful.c) $(wildcard app_vmmux/src/*.c)
APP_INC  = -Iapp_vmmux/include -Iapp_vmmux/faithful
APP_HDRS = $(wildcard app_vmmux/include/*.h app_vmmux/faithful/*.h)
```

`app_vmmux/src/vm_select.c`
```c
#include "plat_tasks.h"
#include "vm_app.h"
const plat_app_t *app_select(void) { return vmmux_app(); }
```

- `make netsim APP=app_vmmux` then builds and runs `build/netsim-app_vmmux`. This is tested on the merged tree.
- The `-Dapp_demo_plat=(*vmmux_app())` line in `app.mk` can go.
- `host/sim_main.c` stays a scripted walkthrough of the demo. Use netsim for this application.

## R3: UI activity flag

`ui_last_touch_ms` (in `ui_app.h`) is the uptime in ms of the last touch on the panel, and
0 if the panel has never been touched. Derive `Touched_Bit` from it in your logic, for example
"touched within the last N s" against the scan's own time.

## R4: FRAM 128 KB

The default map is now 128 KB (`PLAT_FRAM_SIZE` 131072). Your 13.4 KB image is 34 % of
a 40 KB slot. The 32 KB breakout still fits with `PLAT_FRAM_SIZE=32768u`. `make test` runs
both maps. The I2C driver reads the part's Device ID and refuses a smaller part rather
than wrapping.

## R5: target compile check and memory report

`tools/check_target.sh` links `app_vmmux` when `app_vmmux/app_vars.mk` exists and adds its
section to `docs/MEMORY.md`. On the merged tree the whole application compiles for the
STM32H573 with the platform's warnings as errors.

| | Flash | RAM |
|---|---|---|
| Firmware with app_vmmux | 454.5 KB (45 %) | 435.4 KB (68 %) |
| The application itself | 72.8 KB | 47.1 KB |
