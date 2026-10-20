/* ui_app.h - the application's own screens and record editors (decision D26).
 *
 * The application describes WHAT is on its screens, entirely by parameter ID; the
 * platform draws them on the touchscreen and the web page the same way and does every
 * write through the parameter table (all checks, the event log). The application never
 * draws and never sees a touch.
 *
 * ---- Records: whole structures (sequences, schedules) ----
 * A record is a PARAM_RECORD parameter (param.h): a C structure written only whole,
 * applied in one scan after the application's check_block accepted the new image.
 * A ui_record_t describes its fields so the platform can draw an editor:
 *   table  (rows > 0): one row per period; columns are fields with a stride between rows;
 *          rows_off names the "number of rows in use" byte/word (NumberOfPeriods)
 *   form   (rows = 0): a list of fields; a field with count > 1 is a row of tick boxes
 *          (Month[12], Day_Of_Week[7]) or numbers
 * The editor works on a copy; Save posts it (param_record_post) and shows OK or the
 * check's reason; Cancel drops it.
 *
 * ---- Screens ----
 * A ui_screen_t is a list of widgets, laid out by the platform in order:
 *   UW_SECTION  a heading across the screen
 *   UW_VALUE    a value (any scalar parameter, or one element of an array: index);
 *               tap to change it when the login and mode allow
 *   UW_BUTTON   writes cmd_value to cmd_id (a command parameter); lit while state_id
 *               != 0; greyed while enable_id == 0; hidden while show_id < show_min
 *   UW_GRID     a row of numbered items 1..N (valves, inlets): N = count_id's value
 *               (or the length of show_id's array); item i is visible while
 *               show_id[i-1] != 0 (array) and touchable while enable_id[i-1] != 0
 *               (array) or enable_id != 0 (scalar); lit while state_id[i-1] != 0;
 *               a tap writes i to cmd_id; labels from names_id + (i-1) (text
 *               parameters with consecutive IDs), else the number
 *   UW_DIALOG   a question over the screen while show_id >= show_min: label is the
 *               text; cmd_id = Yes/OK (writes cmd_value), cmd2_id = No (0 = OK only)
 *   UW_RECORD   a button that opens record_id's editor
 * Any widget is hidden while show_id < show_min (show_min 0 is taken as 1), so a
 * second tube can be shown only while NumInterconnections >= 2.
 * Commands are written with the touchscreen's or web page's login and source, so
 * PF_CONFIRM ("are you sure?"), levels and LOCAL/REMOTE apply as for any setting.
 *
 * Threading: set once at start-up (plat_start, before the tasks); read-only after. */
#ifndef UI_APP_H
#define UI_APP_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* record fields */
enum { RF_UINT = 0,   /* unsigned integer of ctype (CT_U8, CT_U16, CT_U32) */
       RF_INT,        /* signed integer of ctype (CT_I8, CT_I16, CT_I32) */
       RF_BOOL,       /* one byte 0/1: a tick box */
       RF_TEXT,       /* NUL-terminated 8-bit text of size bytes (printable ASCII) */
       RF_MS_AS_S,    /* uint32 milliseconds, edited as seconds (decimals allowed) */
       RF_HMS,        /* 3 bytes [Seconds, Minutes, Hours] (UniLogic time), edited as hh:mm:ss */
       RF_ENUM };     /* unsigned of ctype, shown as items[value] */

typedef struct {
    const char *label;
    uint8_t     kind;        /* RF_* */
    uint8_t     ctype;       /* RF_UINT/INT/ENUM: CT_* of each element */
    uint16_t    off;         /* offset of the first element (offsetof) */
    uint16_t    stride;      /* table: bytes from one row to the next; form arrays: between elements (0 = packed) */
    uint16_t    count;       /* form: elements (tick boxes in a row); table: ignored (the record's rows) */
    uint16_t    size;        /* RF_TEXT: bytes including the NUL */
    int32_t     min, max;    /* numbers: allowed range (RF_MS_AS_S: in ms) */
    uint16_t    max_param;   /* if set, the maximum is that parameter's current value (e.g. FirstInlets) */
    const char *unit;
    const char *const *items;   /* RF_ENUM: item names, n_items of them */
    uint8_t     n_items;
} ui_field_t;

typedef struct {
    uint16_t          param_id;    /* the PARAM_RECORD */
    const char       *title;
    uint16_t          rows;        /* table: rows (e.g. 20 periods); 0 = a form */
    uint16_t          rows_off;    /* table: offset of the "rows in use" count; 0xFFFF = all rows used */
    uint8_t           rows_ctype;  /* its CT_* (CT_U8 ...) */
    const ui_field_t *fields;
    uint8_t           n_fields;
} ui_record_t;
#define UI_ROWS_ALL 0xFFFFu

/* screen widgets */
enum { UW_SECTION = 0, UW_VALUE, UW_BUTTON, UW_GRID, UW_DIALOG, UW_RECORD };
#define UW_NO_INDEX 0xFFFFu
#define UW_WRITE_ZERO INT32_MIN       /* cmd_value: write 0 (a cmd_value of 0 means 1, a "press") */
static inline int32_t ui_cmd_value(int32_t v) { return v == UW_WRITE_ZERO ? 0 : v ? v : 1; }

typedef struct {
    uint8_t     kind;        /* UW_* */
    const char *label;
    uint16_t    value_id;    /* UW_VALUE: the parameter shown */
    uint16_t    index;       /* UW_VALUE of an array: element (UW_NO_INDEX = whole / scalar) */
    uint16_t    cmd_id;      /* BUTTON, GRID, DIALOG (Yes/OK): the command parameter written */
    int32_t     cmd_value;   /* BUTTON, DIALOG: value written (0 is taken as 1; UW_WRITE_ZERO writes 0) */
    uint16_t    cmd2_id;     /* DIALOG: No (0 = an OK-only message) */
    uint16_t    state_id;    /* BUTTON: lit while != 0; GRID: array, lit item */
    uint16_t    enable_id;   /* touchable while != 0 (0 = always); GRID: array or scalar */
    uint16_t    show_id;     /* shown while value >= show_min (0 = always); GRID: array of visible items */
    int32_t     show_min;
    uint16_t    count_id;    /* GRID: number of items (0 = the length of show_id's array) */
    uint16_t    names_id;    /* GRID: name of item 1 (item i: names_id + i - 1); 0 = numbers */
    uint16_t    record_id;   /* UW_RECORD: the record to edit */
} ui_widget_t;

typedef struct {
    const char        *title;      /* tab label (short: about 12 characters) */
    const ui_widget_t *w;
    uint8_t            n;
} ui_screen_t;

typedef struct {
    const ui_screen_t *screens;    /* shown first, as tabs; the platform's own screens stay */
    size_t             n_screens;
    const ui_record_t *records;
    size_t             n_records;
} ui_app_t;

/* Registration (plat_start does it from plat_app_t; tests call it directly).
 * ui_app_check: 0 if every ID exists and every field fits its record, else the first bad
 * parameter ID (plat_start refuses to run with a bad description, like a bad table). */
void              ui_app_set(const ui_app_t *a);
const ui_app_t   *ui_app(void);
const ui_record_t *ui_app_record(uint16_t param_id);
uint16_t          ui_app_check(const ui_app_t *a);

/* Helpers shared by the touchscreen and the web API (ui_app.c). */
bool    ui_field_get(const ui_field_t *f, const uint8_t *rec, unsigned row, unsigned elem, int64_t *v);
bool    ui_field_set(const ui_field_t *f, uint8_t *rec, unsigned row, unsigned elem, int64_t v);
int32_t ui_field_max(const ui_field_t *f);          /* max, or max_param's current value (if lower) */
unsigned ui_record_rows_used(const ui_record_t *r, const uint8_t *rec);
void    ui_record_set_rows(const ui_record_t *r, uint8_t *rec, unsigned n);
uint16_t ui_field_at(const ui_record_t *r, const ui_field_t *f, unsigned row, unsigned elem);   /* byte offset */
bool    ui_param_value(uint16_t id, uint16_t index, int64_t *v);   /* scalar or array element, as an integer (floats truncated) */

/* UI activity (request R3): ms of uptime of the last touch, 0 = never. */
extern volatile uint32_t ui_last_touch_ms;

#endif
