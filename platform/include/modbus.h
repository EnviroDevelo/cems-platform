/* modbus.h - Modbus server: register map and protocol engine (design spec
 * "Communications": Modbus TCP on port 502 for the PLC / SCADA / DAHS).
 *
 * Everything Modbus can see is an entry of the parameter table, so a Modbus
 * write passes exactly the same checks as a touchscreen or web write (level,
 * local/remote mode, Modbus write enable, range). The map says where each entry
 * appears and in what format. The application supplies the map; it can
 * reproduce the old UniLogic slave map (stage4/4G/modbus_map.csv) so existing
 * DAHS configurations keep working.
 *
 * Function codes: 01 read coils, 02 read discrete inputs, 03 read holding
 * registers, 04 read input registers, 05 write single coil, 06 write single
 * register, 15 write multiple coils, 16 write multiple registers,
 * 43/14 read device identification.
 *
 * Rules
 *  - Reads may start or end inside a multi-register value or a buffer.
 *  - Any address in a request that no entry covers -> exception 02.
 *  - A write must cover whole values (both registers of a 32-bit value) and,
 *    for text entries, the whole entry. Otherwise exception 02 / 03.
 *  - A multi-value write is checked completely before anything is queued: either
 *    all of it is accepted or none of it.
 *  - Writes are applied by the scan (within one scan period). Interlock
 *    refusals at that point are not reported back to the master.
 *
 * Exceptions: 01 illegal function, also when Modbus writes are disabled or the
 * entry needs a higher level; 02 illegal address (unmapped, read-only, partial
 * 32-bit); 03 illegal value (range, type, coil value); 04 refused in the
 * current local/remote mode; 06 busy (write queue full). */
#ifndef MODBUS_H
#define MODBUS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "param.h"

typedef enum { MB_COILS = 0, MB_DISCRETE, MB_HOLDING, MB_INPUT } mb_table_t;

typedef enum {
    MBF_BIT = 0,   /* coils/discrete: one bit from a scalar (non-zero = 1) */
    MBF_U16,       /* registers: one 16-bit register */
    MBF_I16,
    MBF_U32,       /* two registers */
    MBF_I32,
    MBF_F32,       /* IEEE 754 float, two registers */
    MBF_BYTES,     /* a block (bytes/text): two bytes per register */
    MBF_ARRAY      /* an array entry: bools as bits; 8/16-bit elements one register each;
                      32-bit elements and floats two registers each */
} mb_fmt_t;

#define MBF_WORD_LO_FIRST 0x01u   /* 32-bit: low word at the lower address (default: high word first) */
#define MBF_BYTE_LO_FIRST 0x02u   /* blocks: first byte in the low half of the register (default: high half) */

typedef struct {
    uint8_t  table;   /* mb_table_t */
    uint8_t  fmt;     /* mb_fmt_t */
    uint8_t  flags;   /* MBF_* */
    int8_t   dec;     /* float entries shown as MBF_U16/I16/U32/I32: register = value x 10^dec */
    uint16_t addr;    /* first address, 0-based as on the wire */
    uint16_t id;      /* parameter-table ID */
} mb_map_t;

#define MB_ENTRY(TABLE, ADDR, ID, FMT)               { .table = (TABLE), .fmt = (FMT), .addr = (ADDR), .id = (ID) }
#define MB_ENTRY_X(TABLE, ADDR, ID, FMT, FLAGS, DEC) { .table = (TABLE), .fmt = (FMT), .flags = (FLAGS), .dec = (DEC), \
                                                       .addr = (ADDR), .id = (ID) }

typedef struct {
    const char *vendor, *product, *revision;   /* 43/14 device identification */
    uint8_t     write_level;                   /* plat_level_t Modbus writes act with (default operator) */
    uint16_t    map_version;                   /* the application's register map: bump when an address moves
                                                  or a value goes (CIP-14.4); shown at input register 9001 */
} mb_ident_t;

/* The platform's own block, the same in every product (CIP-01.6, 14.4, 15.1, 15.2): input
 * registers 9000-9199 (PLAT_MB_BLOCK_ADDR), added to every map. 0-based addresses:
 *   9000 block layout version (2)       9001 the application's map version (mb_ident_t)
 *   9002 heartbeat: +1 every 100 ms      9003 instrument state (PLAT_ST_*)
 *   9004 s since the last Modbus or web request (65535 = none, or longer)
 *   9005 alarms active                   9006 common alarm bits (ACOM_*)
 *   9007-9008 hardware revision (U32)    9009-9010 serial number (U32)
 *   9011-9012 firmware version (U32, major<<16 | minor<<8 | patch)   9013 web API version
 *   9014-9015 uptime, s (U32)            9016-9017 supply, mV (U32; 0 = not measured)
 *   9018-9019 state entered (U32, local s since 1970)
 *   9020-9051 quality of measurement 0..31 (PLAT_Q_*)
 *   9052-9067 product (32 bytes text)    9068-9075 vendor (16)    9076-9083 firmware version (16)
 *   layout 2 (added; nothing above moved):
 *   9084-9085 clock (U32, local s since 1970; 0 = not set)   9086 clock synchronised (0/1)
 *   9087 clock source (PLAT_CLK_SRC_*)  9088-9089 clock last set or confirmed (U32)
 * 32-bit values high word first; text two bytes per register, first byte high, NUL padded. */
#define MB_BLOCK_REGS 90u
void mb_block_scan(uint32_t now_ms);     /* scan task: heartbeat, link age, uptime */

/* Build the map index: the platform's block plus the application's map. Checks that every ID
 * exists, the format suits the entry, and no two entries in a table overlap (an application
 * entry in 9000-9199 of the input registers overlaps the block). Returns 0, or the index of the
 * first bad application entry + 1 (call after param_init). */
int  mb_init(const mb_map_t *map, size_t n, const mb_ident_t *ident);
uint16_t mb_entry_span(const mb_map_t *e);    /* addresses (bits or registers) an entry covers; 0 if invalid */

/* Process one request PDU (function code + data). Writes the response PDU
 * (normal or exception) to resp and returns its length. */
uint16_t mb_pdu(const uint8_t *req, uint16_t len, uint8_t *resp, uint16_t cap);

/* Statistics */
typedef struct { uint32_t requests, exceptions, writes_queued; } mb_stats_t;
mb_stats_t mb_stats(void);

#endif
