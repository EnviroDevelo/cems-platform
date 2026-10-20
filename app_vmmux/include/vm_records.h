/* vm_records.h - the sequence / schedule records (vm_records.c). */
#ifndef VM_RECORDS_H
#define VM_RECORDS_H
#include <stdint.h>
#include <stddef.h>

/* The application check the scan calls before applying a record write (core design, 20 Oct).
 * 0 = accept; otherwise nothing changes and why holds the reason for the operator. */
int vm_check_record(uint16_t id, const void *new_image, char *why, size_t cap);

/* Field kinds for the editors */
enum {
    VRF_ROWNUM = 0,  /* uint8 row number (written by the editor: 1..n, 0 after) */
    VRF_TEXT,        /* char[13] per row, printable, up to 12 characters */
    VRF_MS_AS_S,     /* uint32 ms, shown and entered as seconds */
    VRF_U8,          /* uint8 1-based valve number, 1..max_param */
    VRF_INLET0,      /* uint8 0-based MUX inlet index, shown as 1..(fitted inlets) */
    VRF_BOOL,        /* bool per row */
    VRF_U16,         /* uint16 */
    VRF_TICKS,       /* bool[max_value] tick boxes */
    VRF_HMS,         /* uint8[3] = seconds, minutes, hours */
};
enum { VRK_TABLE = 0, VRK_FORM = 1 };

typedef struct {
    const char *label;
    uint8_t     kind;          /* VRF_* */
    uint16_t    offset, size;  /* bytes in the record (table fields: the whole 20-element array) */
    uint16_t    max_param;     /* live parameter that limits the value (0 = none) */
    uint16_t    max_value;     /* fixed limit / element count (0 = none) */
} vm_rec_field_t;

typedef struct {
    uint16_t    id;
    const char *title;
    uint8_t     kind;          /* VRK_* */
    uint16_t    size;          /* bytes of the whole record */
    int16_t     rows_offset;   /* table: offset of the uint8 row count (NumberOfPeriods); -1 for forms */
    const vm_rec_field_t *fields;
    uint8_t     n_fields;
} vm_record_t;

extern const vm_record_t vm_records[];
extern const size_t vm_n_records;
#endif
