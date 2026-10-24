/* mb_rtu.h - Modbus RTU framing for the RS-485 master (iobus.h).
 *
 * Builds request frames (address, PDU, CRC) and checks reply frames. Pure C with no
 * hardware or OS calls, so the I/O node firmware can share it (I/O module requirements
 * document) and the host tests run it unchanged.
 *
 * Functions: 03 read holding registers, 06 write single register, 16 write multiple
 * registers, 23 read/write multiple registers (the write is done before the read).
 * Registers are big-endian on the wire, CRC-16/MODBUS low byte first. */
#ifndef MB_RTU_H
#define MB_RTU_H
#include <stdint.h>
#include <stddef.h>

#define MBR_FRAME_MAX 256u           /* the largest RTU frame */
#define MBR_REGS_MAX  123u           /* most registers one FC16 may write */

/* reply check results */
enum {
    MBR_OK = 0,
    MBR_NO_REPLY,      /* nothing came back before the timeout */
    MBR_SHORT,         /* fewer bytes than the reply needs */
    MBR_CRC,           /* CRC wrong (noise, collision, wrong baud rate) */
    MBR_WRONG_ADDR,    /* another slave answered */
    MBR_WRONG_FUNC,    /* reply to another function */
    MBR_BAD_FORMAT,    /* byte count or echo does not match the request */
    MBR_EXCEPTION      /* the slave answered with an exception (code in *exc) */
};

uint16_t mbr_crc16(const uint8_t *p, size_t n);

/* Request builders: write the whole frame (with CRC) to f, return its length
 * (0 if the arguments are out of range). */
size_t mbr_req_read(uint8_t *f, uint8_t addr, uint16_t start, uint16_t n);                       /* 03 */
size_t mbr_req_write1(uint8_t *f, uint8_t addr, uint16_t reg, uint16_t v);                       /* 06 */
size_t mbr_req_write(uint8_t *f, uint8_t addr, uint16_t start, const uint16_t *v, uint16_t n);   /* 16 */
size_t mbr_req_rw(uint8_t *f, uint8_t addr, uint16_t rd_start, uint16_t rd_n,
                  uint16_t wr_start, const uint16_t *wv, uint16_t wr_n);                         /* 23 */

/* Check a reply to request req (as built above). Reads (03, 23): the registers go to
 * regs (n_regs of them, as asked). Writes (06, 16): the echo must match. Returns MBR_*;
 * for MBR_EXCEPTION *exc is the exception code (exc may be NULL). */
int mbr_check_reply(const uint8_t *req, size_t req_len, const uint8_t *rx, size_t rx_len,
                    uint16_t *regs, uint16_t n_regs, uint8_t *exc);

const char *mbr_result_name(int r);

#endif
