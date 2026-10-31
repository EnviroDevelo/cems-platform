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
 *   layout 3 (added): 9090 values forced on Modbus now (mb_force_*; 0 = none)
 * 32-bit values high word first; text two bytes per register, first byte high, NUL padded. */
#define MB_BLOCK_REGS 91u
/* The standard CEMS block (VDI 4201-3, docs/MODBUS_STANDARD_BLOCK.md): input registers 7000-8999
 * (PLAT_MB_STD_ADDR), the same in the holding registers, read-only (writes: exception 02), built
 * from the emissions engine's parameters on each read; slots without one read NaN. Word order:
 * param 335 mb_std_words. Its forceable values are the engine's banks (450-457). 43/14 gives
 * its self-description (conformity 0x83). A product's map may not use 7000-8999 (input, holding) or coils 8000-8099. */
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
/* The map in force (the platform's block and the application's), sorted by table and address:
 * for the register map document and the interface snapshot (tools/modbus_map.c). */
size_t mb_entry_count(void);
const mb_map_t *mb_entry_at(size_t i, uint16_t *span);   /* NULL past the end */
mb_stats_t mb_stats(void);

/* Forced values (commissioning, Gary 29 Oct): at admin login, from the touchscreen or the web, a
 * value the PLC reads can be forced for PLAT_MB_FORCE_MS, so the control system can be checked
 * against known values; then it reverts by itself. A force changes ONLY what Modbus serves:
 * the screens, alarms, logs and control keep the real value. Test bits are forced bools.
 *  - Forceable (mb_forceable): a numeric or bool parameter (or array element) on Modbus that is
 *    read-only to Modbus (input registers or discrete inputs only, or a PF_RO parameter); never a
 *    setting, a text, the block's versions, the identity (serial, revisions) or the clock.
 *  - Any LOCAL/REMOTE mode. Setting it again restarts the time. Not kept over a restart.
 *  - The master can tell: input register 9090 counts the forces, common alarm bit ACOM_FORCED.
 *  - Every set, clear and expiry is in the event log (LOG_EV_FORCE).
 * Any task: set/clear from the web or touchscreen task, expiry in mb_block_scan (scan task). */
#ifndef PLAT_MB_FORCE_MS
#define PLAT_MB_FORCE_MS 30000u
#endif
#define MB_FORCE_MAX     16u
#define MB_FORCE_SCALAR  0xFFFFu        /* index of a value that is not an array */
typedef struct { uint16_t id, index; double v; uint32_t left_ms; uint8_t src, level; } mb_force_t;
bool     mb_forceable(uint16_t id);
/* param_err_t: OK; PERR_ACCESS below admin or from Modbus; PERR_NO_ID not forceable; PERR_RANGE bad
 * index, or v not a value of the parameter's type (range limits do not apply: an out-of-range value
 * can be simulated); PERR_BUSY MB_FORCE_MAX values forced already. Bools: v != 0. */
int      mb_force_set(uint16_t id, uint16_t index, double v, uint8_t src, uint8_t level, uint32_t now_ms);
int      mb_force_clear(uint16_t id, uint16_t index, uint8_t src, uint8_t level);   /* PERR_NO_ID: not forced */
int      mb_force_clear_all(uint8_t src, uint8_t level);
unsigned mb_force_count(void);
bool     mb_force_active(uint16_t id, uint16_t index);   /* index MB_FORCE_SCALAR for a scalar */
bool     mb_force_get(unsigned i, mb_force_t *f, uint32_t now_ms);   /* i < mb_force_count() */

#endif
