# Modbus TCP: guide for PLC, SCADA and DAHS integrators (CIP-26.7)

This page covers what is the same on every product built on the CEMS core: the connection,
the function codes, the rules for reads and writes, and the platform block at input registers
9000-9089. Each product's own registers are in its generated map, for example
[`interface/app_demo/MODBUS_MAP.md`](interface/app_demo/MODBUS_MAP.md) for the demo application.
That file is produced from the firmware's tables by `make modbus-map`, so it can't drift from
what the unit actually serves.

Code: `platform/core/modbus.c` (the protocol engine and the block), `platform/net/mbtcp.c`
(the TCP server), `platform/include/modbus.h` (the full rules). Compatibility promises:
[`INTERFACES.md`](INTERFACES.md) 2.

## 1. Connection

| Item | Value |
|---|---|
| Transport | Modbus TCP (MBAP framing), on the client/plant Ethernet port. The I/O board port never serves Modbus. |
| Port | 502 |
| Unit ID | any value is accepted and echoed in the reply. Use 1 if the master needs a value. |
| Clients at once | 4. A fifth connection replaces the client that has been silent longest, so a PLC that restarted without closing its socket can't hold a slot for ever. |
| Allowed client | parameter 5 `modbus_allow_ip`: 0 accepts any address; otherwise only that one IP may connect, and others are closed at once. |
| Keep-alive | TCP keep-alive is on. There is no idle timeout of its own; keep one connection open and poll on it. |
| Requests in flight | several requests may be pipelined on one connection; they are answered in order. |
| Bad framing | a protocol ID other than 0, or a length outside 2-254, closes that connection. |
| Response time | reads are answered from the latest values without waiting for the scan. Writes are acknowledged once checked and queued, and applied within one scan period (section 4). |

The unit's IP address, mask and gateway are set in the web page (Settings, admin) and shown on
the touchscreen's System screen. Modbus can't change network or security settings (they are flagged "never from Modbus").

Status for commissioning: the web page's Service tab, or `GET /api/info` `"modbus"` (clients
connected, requests, exceptions, writes queued, refused clients, seconds since the last request).

## 2. Function codes

| Code | Function | Limit per request |
|---|---|---|
| 01 | Read coils | 2000 bits |
| 02 | Read discrete inputs | 2000 bits |
| 03 | Read holding registers | 125 registers |
| 04 | Read input registers | 125 registers |
| 05 | Write single coil (FF00 = on, 0000 = off) | 1 |
| 06 | Write single register | 1 |
| 15 | Write multiple coils | 1968 bits |
| 16 | Write multiple registers | 123 registers |
| 43 / 14 | Read device identification: basic objects 0 vendor, 1 product, 2 firmware version; read codes 1 (stream) and 4 (one object) | |

Any other function code gets exception 01.

## 3. Addresses and formats

- Addresses are **0-based, as on the wire**. A SCADA package that counts from 1 (40001 style)
  needs 1 added: holding register 20 is 40021, input register 9000 is 39001 (or 309001 in
  6-digit numbering).
- **32-bit values** (u32, i32, f32) take two registers, **high word first**. An entry marked
  "low word first" in the product's map swaps them. Floats are IEEE 754 single precision.
- **Scaled values**: an entry with "value x 10^n" carries a real number as an integer. Divide
  the register by 10^n (for example `demo_level`, i16 with n = 1: 123 means 12.3 %). Values
  outside the register's range are clamped to its limits, and a value that isn't a number reads 0.
- **Text and byte blocks**: two bytes per register, first byte in the high half (unless the
  map says "low byte first"), padded with NUL bytes. A full-length text has no terminator.
- **Arrays**: booleans are one bit each (coils or discrete inputs); 8- and 16-bit elements are
  one register each; 32-bit elements and floats two registers each.
- **Reads** may start or end in the middle of a 32-bit value, a text or an array. Every address
  in the request must belong to some entry, otherwise the whole request gets exception 02. So
  read the gaps between entries separately, or not at all.

## 4. Writes

A Modbus write goes through exactly the same checks as a touchscreen or web change. For a write
to be accepted, all of these must hold:

1. **Modbus writes are enabled**: parameter 4 `modbus_write_en` (default on, admin to change).
   When it is off, every write gets exception 01. Reads always work.
2. **The unit is in REMOTE mode.** It starts in LOCAL, and only the touchscreen switches
   between them. In LOCAL, writes get exception 04. A few entries are writable in any mode; they
   are marked in the product's map. The alarm acknowledge (holding register `alarm_ack`) is one.
3. **The entry allows Modbus at operator level.** Modbus writes with operator rights and has no
   login. Entries that need supervisor, admin or factory, and entries flagged "never from
   Modbus" (network, security, passwords, records), get exception 01.
4. **The value is valid**: within the entry's range, of the right type, a coil value of FF00 or
   0000. Otherwise exception 03.
5. **Whole values only**: both registers of a 32-bit value, and a whole text entry. A write that
   covers half a value gets exception 02 or 03.

A multiple write (15 or 16) is checked **completely before anything is applied**: either all of
it is accepted or none of it is. A positive reply means "checked and queued". The scan applies the
write within one scan period. A refusal at that point (an interlock that changed in between) is
counted in the scan's rejected commands, but isn't reported to the master. Read the value back if
you need to be sure.

Every change a write makes is recorded in the event log with source "modbus", the same as for
the touchscreen and the web.

Acknowledging alarms: write an alarm ID to `alarm_ack` (65535 acknowledges all). This works in
any mode, with no login.

## 5. Exceptions

| Code | Name | When |
|---|---|---|
| 01 | Illegal function | unknown function code; Modbus writes disabled; the entry needs a higher level than operator or is never writable from Modbus |
| 02 | Illegal data address | an address no entry covers; a write to a read-only entry; half of a 32-bit value |
| 03 | Illegal data value | a bad length or count, a value out of range, the wrong type, a coil value other than FF00/0000 |
| 04 | Server device failure | refused in the current mode (LOCAL), or by an interlock at the time of the check |
| 06 | Server busy | the write queue is full; retry after one scan period |

## 6. The platform block (input registers 9000-9089)

The same addresses in every product (CIP-01.6, 14.4, 15.1, 15.2). Input registers 9000-9199 are
reserved for it; a product's own map can't use them.

| Register | Size | Contents |
|---|---|---|
| 9000 | 1 | block layout version: **2** |
| 9001 | 1 | the product's own map version (changes when an address in the product's map moves or a value is removed) |
| 9002 | 1 | heartbeat: +1 every 100 ms, wraps at 65535. If it stops changing, the core's scan has stopped. |
| 9003 | 1 | instrument state (table below) |
| 9004 | 1 | seconds since the last Modbus or web request (65535 = none since start, or longer) |
| 9005 | 1 | number of alarms active |
| 9006 | 1 | common alarm bits (table below) |
| 9007-9008 | 2 | hardware revision (u32) |
| 9009-9010 | 2 | serial number (u32) |
| 9011-9012 | 2 | firmware version (u32: major × 65536 + minor × 256 + patch) |
| 9013 | 1 | web API version |
| 9014-9015 | 2 | uptime, seconds (u32) |
| 9016-9017 | 2 | supply voltage, mV (u32; 0 = not measured) |
| 9018-9019 | 2 | time the present state was entered (u32, local seconds since 1970) |
| 9020-9051 | 32 | quality of measurement 0..31 (table below); unused measurements read 0 |
| 9052-9067 | 16 | product name (32 bytes of text) |
| 9068-9075 | 8 | vendor (16 bytes of text) |
| 9076-9083 | 8 | firmware version as text (16 bytes) |
| 9084-9085 | 2 | the unit's clock (u32, local seconds since 1970; 0 = not set) |
| 9086 | 1 | clock synchronised: 1 = set and confirmed recently |
| 9087 | 1 | clock source: 0 none, 1 SNTP, 2 touchscreen, 3 web, 4 Modbus |
| 9088-9089 | 2 | when the clock was last set or confirmed (u32, local seconds since 1970) |

Layout 2 added 9084-9089; nothing in layout 1 moved. A master should read 9000 once at connect,
and use only the registers that layout has.

**Instrument state (9003)**

| Value | State | Meaning for the data |
|---|---|---|
| 0 | starting | not yet measuring |
| 1 | initialising | not yet measuring |
| 2 | warming up | measurements flagged "warm-up" |
| 3 | ready | idle, fit to measure |
| 4 | measuring | normal operation |
| 5 | maintenance | measurements flagged "maintenance": exclude from reported data |
| 6 | calibration | measurements flagged "calibration": exclude from reported data |
| 7 | fault | a fault stops measurement; see the alarms |
| 8 | safe mode | started with the defaults after repeated failed starts; service needed |

**Measurement quality (9020-9051)**

| Value | Quality | Use the value? |
|---|---|---|
| 0 | valid | yes |
| 1 | stale | no: not updated within its expected period |
| 2 | unavailable | no: no reading (sensor or I/O board missing) |
| 3 | out of range | no: outside the instrument's measuring range |
| 4 | warm-up | no |
| 5 | maintenance | no |
| 6 | calibration | no |
| 7 | fault | no |

The product's map says which measurement is which index. The same quality also appears beside
each value in the web page and on the touchscreen (UI_CONVENTIONS.md).

**Common alarm bits (9006)**

| Bit | Mask | Meaning |
|---|---|---|
| 0 | 0x0001 | an alarm routed to Modbus is active |
| 1 | 0x0002 | an alarm routed to Modbus waits for an acknowledge |
| 2 | 0x0004 | any alarm is active |
| 3 | 0x0008 | any alarm waits for an acknowledge |
| 8 + k | 0x0100 << k | alarm relay k is in alarm (before any inversion) |

Which alarms count in bits 0 and 1 is set per alarm in the web page's Alarm setup tab (the
Modbus column). Every alarm's own state is always readable, routed or not.
The alarm IDs, and the per-alarm active and unacknowledged bits, are in the product's map
(`alarm_active_bits`, `alarm_unack_bits`) and in [`FAULT_CODES.md`](FAULT_CODES.md).

## 7. Supervising the link

- **From the master's side**: watch the heartbeat (9002). If it hasn't changed for 1 s or more,
  the unit isn't scanning, even if it still answers.
- **From the unit's side**: parameter 3 `comms_timeout` (default 10 s; 0 = off). In REMOTE mode,
  if no Modbus or web request arrives within it, the unit raises alarm 5 "remote link lost".
  Poll at least every few seconds to keep it clear. What the product does beyond the alarm is
  given in its own documentation.

## 8. Commissioning checklist

1. Set the unit's IP in the web page. Optionally set `modbus_allow_ip` to the PLC's address.
2. Read 9000-9019 (function 04): layout 2, the map version you configured for, a heartbeat that
   moves, state 3 or 4.
3. Read 9052-9067 and check the product name. Or use 43/14.
4. Read the product's own registers from its map.
5. For writes: put the unit in REMOTE on the touchscreen, write one value, read it back, and find
   it in the web page's Event log tab.
6. Check the web Service tab's Modbus panel: your connection is listed, and exceptions don't rise
   in normal polling.

## 9. Not offered on the core

- **Modbus RTU / RS-485**: removed from the core board (decision D27, revised 24 Oct). The I/O
  boards' bus is internal and isn't for the plant.
- **Modbus security (TLS on 802)**: not offered. Use `modbus_allow_ip`, `modbus_write_en`
  and the site's network segregation (CIP-14.5 policy).
