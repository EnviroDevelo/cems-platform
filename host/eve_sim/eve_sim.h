/* eve_sim.h - a BT817 stand-in on the eve_bus_t interface, for host tests and the
 * UI screenshots: decodes the SPI framing, keeps the register file, executes the
 * co-processor list instantly and keeps the last complete frame. Touch is injected
 * by setting the tag under the "finger". */
#ifndef EVE_SIM_H
#define EVE_SIM_H
#include <stdio.h>
#include "eve.h"

typedef struct {
    eve_bus_t bus;
    uint8_t   regs[0x1000];         /* 0x302000.. */
    uint8_t   err[128];             /* RAM_ERR */
    uint16_t  patch_ptr;
    bool      present;              /* answers at all */
    bool      fault_next;           /* the next frame faults the co-processor */
    uint32_t  host_cmds, last_host_cmd;
    /* SPI transaction being decoded */
    bool      cs;
    uint8_t   hdr[4];
    uint32_t  hn, addr;
    bool      is_write, did_read;
    /* co-processor stream */
    uint8_t   cur[16384];
    uint32_t  cur_n;
    uint8_t   frame[16384];         /* last complete frame (DLSTART .. SWAP) */
    uint32_t  frame_n, frames;
    /* memories and the out-of-frame commands (CMD_INFLATE, CMD_MEMCRC) */
    uint8_t  *ram_g;                /* 1 MB (shared by every simulator: tests run one at a time) */
    uint8_t   ram_cmd[4096];        /* the FIFO as the chip holds it (results are read back from it) */
    uint32_t  op;                   /* the out-of-frame command being received (0 = none) */
    uint32_t  op_args[3], op_n;
    uint8_t   opw[4];
    uint32_t  opw_n;
    uint8_t  *zbuf;                 /* CMD_INFLATE's compressed stream so far */
    uint32_t  zn;
    uint32_t  inflates, memcrcs, inflate_errors;
} eve_sim_t;

void eve_sim_init(eve_sim_t *s);
void eve_sim_touch(eve_sim_t *s, uint8_t tag);            /* 0 = finger lifted */
void eve_sim_touch_xy(eve_sim_t *s, int16_t x, int16_t y);   /* REG_TOUCH_SCREEN_XY; -32768, -32768 = no finger */

/* Inspect the last frame */
bool eve_sim_has_text(const eve_sim_t *s, const char *needle);   /* in any text/button/keys string */
int  eve_sim_tag_of(const eve_sim_t *s, const char *label);      /* tag in force for a button/text with exactly this label; -1 */
void eve_sim_dump(const eve_sim_t *s, FILE *f);                  /* raw frame for tools/eve_preview.py */
void eve_sim_print(const eve_sim_t *s, FILE *f);                 /* readable listing */

#endif
