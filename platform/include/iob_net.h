/* iob_net.h - the I/O boards on Ethernet (decision D27, revised 24 Oct): Modbus TCP and the board
 * discovery protocol. Shared by the supervisor (the master, iobus.c / iob_tcp.c) and the I/O board
 * firmware (io-node), like iobus.h and mb_rtu.h. Pure C, no sockets.
 *
 * Modbus TCP. The register map (iobus.h) and the requests are the same as on RS-485; only the
 * framing changes. The master builds RTU frames as before (mb_rtu.h) and iob_mbap_from_rtu() turns
 * one into a Modbus TCP frame (MBAP header, unit id = the board's Modbus address, no CRC). The
 * board turns it back into the RTU frame its core already handles (iob_rtu_from_mbap) and the reply
 * goes the same way back. So every rule of the RS-485 link (addresses, write rules, hold time, comms
 * watchdog, input change reporting) holds unchanged over Ethernet. Port IOB_TCP_PORT; the board
 * serves one connection at a time (a new one replaces the old: the supervisor restarted) and, once
 * it has been configured, only from the supervisor's IP.
 *
 * Discovery and IP settings: UDP, port IOB_DISC_PORT, IOB_DISC_LEN-byte messages, big-endian.
 *   FIND  supervisor -> broadcast        every board answers HERE (to the broadcast address, so the
 *                                        answer arrives whatever subnet the board is set to)
 *   HERE  board -> broadcast             its identity (UID, MAC, serial, firmware, hardware) and its
 *                                        settings (IP, mask, gateway, supervisor IP, Modbus address)
 *   SET   supervisor -> broadcast        new settings for the board whose UID matches; refused
 *                                        (ignored) when the board already has a supervisor IP and
 *                                        the request comes from another address. The board saves
 *                                        the settings, reads them back, answers HERE with them,
 *                                        then changes over to the new IP.
 * A new board has IP 0 (not set): it answers FIND and SET but serves no Modbus TCP. Clearing a
 * board back to new is done over its USB port (tools/set_address.py --clear). */
#ifndef IOB_NET_H
#define IOB_NET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IOB_TCP_PORT     502u
#define IOB_DISC_PORT    50223u
#define IOB_DISC_LEN     80u
#define IOB_DISC_MAGIC   "CEMSIOB1"         /* 8 bytes, not NUL terminated on the wire */
#define IOB_MBAP_MAX     260u               /* MBAP 7 + PDU 253 */

enum { IOB_DISC_FIND = 1, IOB_DISC_HERE = 2, IOB_DISC_SET = 3 };

#define IOB_DISC_F_NEW        0x0001u       /* HERE: the board has no IP yet */
#define IOB_DISC_F_CONTROLLED 0x0002u       /* HERE: a master is talking to it now (outputs may be on) */

typedef struct {
    uint8_t  op;              /* IOB_DISC_* */
    uint16_t seq;             /* FIND/SET: chosen by the supervisor; HERE: echoes the request's */
    uint16_t uid[6];          /* HERE: the board's; SET: the board it is for */
    uint8_t  mac[6];          /* HERE */
    char     serial[17];      /* HERE: printable ASCII, NUL padded ("" = not set) */
    uint16_t fw_version, fw_build, hw_rev, flags;   /* HERE */
    uint32_t ip, mask, gw;    /* HERE: current; SET: new. Host byte order */
    uint32_t master_ip;       /* HERE: current; SET: new (the supervisor's own IP) */
    uint8_t  address;         /* HERE: current; SET: new Modbus address (unit id), 1..IOB_ADDR_MAX */
} iob_disc_t;

/* IOB_DISC_LEN bytes into buf; returns IOB_DISC_LEN */
size_t iob_disc_encode(const iob_disc_t *m, uint8_t *buf);
/* false: not a discovery message (wrong length, magic or op) */
bool   iob_disc_decode(const uint8_t *buf, size_t len, iob_disc_t *m);

/* The settings a SET may carry: a usable unicast IP inside the mask's subnet with the supervisor in
 * the same subnet, a contiguous mask of /8 to /30, a gateway in the subnet or 0, address 1..245. */
bool   iob_net_settings_ok(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t master_ip, uint8_t address);

/* The board's rule for a SET (both the firmware and the simulated board use it): the UID is its own,
 * the settings are usable (iob_net_settings_ok), and if the board already has a supervisor IP the
 * request comes from that address. */
bool   iob_disc_set_accept(const iob_disc_t *set, const uint16_t uid[6], uint32_t cur_master_ip, uint32_t from_ip);

/* RTU frame (address, PDU, CRC) -> Modbus TCP frame with transaction id tid. Returns its length,
 * 0 if the RTU frame is too short or its CRC is wrong. out holds IOB_MBAP_MAX bytes. */
size_t iob_mbap_from_rtu(const uint8_t *rtu, size_t len, uint16_t tid, uint8_t *out);
/* Modbus TCP frame -> RTU frame (unit id as the address, CRC added). Returns its length, 0 if the
 * frame is not one complete Modbus TCP frame. *tid gets its transaction id (tid may be NULL).
 * out holds IOB_MBAP_MAX bytes. */
size_t iob_rtu_from_mbap(const uint8_t *mbap, size_t len, uint16_t *tid, uint8_t *out);
/* Bytes of a complete Modbus TCP frame at the start of a stream buffer: 0 = not complete yet,
 * -1 = not Modbus TCP (close the connection). */
int    iob_mbap_complete(const uint8_t *buf, size_t len);

/* A locally administered MAC from the chip's unique ID (02:xx:xx:xx:xx:xx), when the board has no
 * EUI-48 chip. */
void   iob_mac_from_uid(const uint16_t uid[6], uint8_t mac[6]);

#endif
