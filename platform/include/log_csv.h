/* log_csv.h - turns raw log slots (plat_log_read_*) into CSV text, streaming:
 * feed the slots in order, oldest first, in chunks of any size.
 *
 * Data CSV:   time,<ch> [unit],<ch> q,...   one row per interval; a new header
 *             line is written whenever the channel set or interval changed
 *             (old faults L3/L4: the log is never cleared to change columns).
 *             Quality: OK / FLAG / BAD / N/A. A value with no valid sample is empty.
 *             A power-off gap (CIP-16.6): when two consecutive rows with the clock set
 *             are more than 2 intervals apart, one row is written at the first missing
 *             interval with every value empty and every quality GAP; the data resume
 *             at the next real row. (A forward clock step shows the same way.)
 * Event CSV:  seq,time,event,source,level,id,detail,old,new
 *
 * Times are "YYYY-MM-DD hh:mm:ss" (the RTC's time zone), or "uptime <n>s" for
 * records made while the clock was not set. */
#ifndef LOG_CSV_H
#define LOG_CSV_H
#include <stdint.h>
#include <stdbool.h>
#include "plat_log.h"
#include "http_util.h"

typedef struct {
    uint8_t  ring;
    uint32_t from, to;                 /* time filter, inclusive; 0 = open */
    uint32_t last_seq;
    bool     have_seq;
    /* record being assembled */
    uint8_t  rec[17 * LOG_SLOT];
    unsigned have, need;
    /* current schema */
    bool     schema_ok, header_out;
    uint16_t hash, iv;
    unsigned nch;
    char     name[LOG_MAX_CHANS][17];
    char     unit[LOG_MAX_CHANS][7];
    uint8_t  dec[LOG_MAX_CHANS];
    /* the running configuration, used when the schema record has been overwritten */
    const plat_log_chan_t *live;
    unsigned n_live;
    uint16_t live_iv;
    uint32_t rows, skipped;
    uint32_t last_t;                   /* time of the last row written, for gap rows */
    bool     have_last;                /* last_t is valid: same header, clock set */
} log_csv_t;

void log_csv_begin(log_csv_t *c, uint8_t ring, const plat_log_chan_t *live, unsigned n_live,
                   uint16_t live_iv, uint32_t from, uint32_t to, http_out_t *o);
void log_csv_feed(log_csv_t *c, const uint8_t *slots, uint32_t n, http_out_t *o);

void log_fmt_time(char *buf, size_t cap, uint32_t t, bool clock_set);  /* cap >= 24 */
void log_fmt_fixed(char *buf, size_t cap, double v, unsigned dec);     /* no printf %f on the target */
const char *log_src_name(uint8_t src);
/* plain words for an event, as the web page shows it: what ("Setting changed") and the
 * detail ("start_delay (1001): 2000 -> 1500") */
void log_event_text(const log_event_t *e, char *what, size_t wcap, char *detail, size_t dcap);
const char *log_level_name(uint8_t level);

#endif
