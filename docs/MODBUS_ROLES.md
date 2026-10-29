# Modbus on the CORE: three separate roles

Rev A, 29 Oct 2026. The CORE uses Modbus TCP in three different ways. They run on different
networks, with the CORE in a different role in each, and they share no settings, so a change to one
never touches another. This page says what each role is, which standard it follows, and how role 2
(third-party instruments) is built.

| | 1. I/O boards | 2. Instruments (analysers etc.) | 3. Client output |
|---|---|---|---|
| What | our own CEMS I/O boards | gas analysers, flow, dust, opacity, any third-party device | the site's PLC, SCADA or DAHS |
| CORE is | **master** (client) | **master** (client) | **slave** (server) |
| Network | I/O port (W5500, RJ45 #1), private subnet 192.168.1.x | the same I/O port, beside the boards | client port (STM32 MAC, RJ45 #2), the site's network |
| Who defines the registers | us (`iobus.h`, map version 2) | each instrument's maker | us (project map + standard block 7000-7999 + platform block 9000-9199) |
| Set up by | scan or discovery, then IP, node address, channel range (web page, I/O boards tab) | a point list per instrument (parameters 500-621, web page) | fixed per product, handed over as the Excel map |
| Code | `iobus.c`, `iob_net.c`, `iob_w5500.c` | `inst.c`, `inst_w5500.c` (this change) | `modbus.c`, `mbtcp.c` |
| W5500 sockets | 0-2 (boards), 3 (discovery) | 4-6 (instruments), 7 (register explorer) | none (lwIP on the client port) |

## Industry standard, applied to all three

Gary asked (29 Oct) for Modbus to follow industry standard as far as it can. The rules every role
keeps:

- **The specifications.** *Modbus Application Protocol Specification V1.1b3* and *Modbus Messaging
  on TCP/IP Implementation Guide V1.0b* (modbus.org): TCP port 502, the MBAP header (transaction id,
  protocol id 0, length, unit id), one PDU per frame, at most 253 bytes of PDU.
- **Only public function codes.** 01-06, 15, 16, 23 and 43/14 (device identification). No
  user-defined function codes anywhere, so any standard Modbus tool (Modbus Poll, QModMaster,
  pymodbus, a PLC's own driver) can talk to every role.
- **Addresses.** On the wire they are 0-based (PDU addresses), as the specification says.
  Documents and the Excel handover give both: the 0-based address and the traditional Modicon
  reference (input register 7000 = 37001, holding 7000 = 47001), because integrators still use both.
- **Data.** Registers are 16-bit big-endian (the specification). The specification does not define
  32-bit values; the de facto standard is IEEE 754 float or 32-bit integer in two registers, high
  word first ("ABCD", big-endian). That is what the CORE sends (role 3) and the default it reads
  (role 2). Other orders are only for reading devices that use them.
- **Exceptions.** The standard codes only: 01 function, 02 address, 03 value, 04 device failure,
  06 busy, and 0A/0B from gateways.
- **Status and quality.** NAMUR NE 107 categories (failure, function check, out of specification,
  maintenance) for the status words we publish, and the eight platform quality codes behind them
  (`plat_state.h`).

What each role adds to that:

1. **I/O boards.** Already standard Modbus: holding registers read with 03/04, written with 06/16,
   and 23 (read/write multiple) for the cycle. Finding a board needs something Modbus does not
   have, so discovery is a small UDP message set beside it (`iob_net.h`, port 50223), the same way
   gateway makers ship their own finder tools. A board answers ordinary Modbus TCP as soon as it has
   an IP, so it can be checked with any Modbus tool.
2. **Instruments.** The CORE reads with 01-04 only and follows the TCP implementation guide's
   client rules: one connection per device (or per gateway), one transaction outstanding at a time,
   transaction ids checked so a late answer is never taken for the next one, unit id = the
   instrument's own address behind a gateway (1 when it is a direct Modbus TCP device; set 255 for
   devices that ask for it). The published standard for a measuring instrument's digital interface
   to a data acquisition system is VDI 4201 (Part 1 general, Part 3 Modbus), referenced by EN 17255
   and EN 15267-3; newer certified analysers implement it, older ones and most other devices publish
   their own maps. So this role stays configurable (the only place the CORE accepts non-standard
   word orders), and a VDI 4201 profile is the first profile to write once the standard text is
   bought (the Modbus map thread has asked Gary for it).
3. **Client output.** The proposed standard block (input registers 7000-7999, Modbus standard map
   thread) puts the common values (O2, CO, NOx, SO2, dust, flow, temperature ...) at fixed addresses
   in every product, in IEEE floats high word first, with a NAMUR status word per value, the same way
   ABB, SICK, Thermo and Emerson give their analysers one fixed map. Project-specific values stay in
   0-6999. Holding registers mirror the input registers for PLCs that can only read 03.
   The block's layout is provisional until it is checked against VDI 4201 Part 3, the published
   CEMS-to-DAHS Modbus standard (Modbus map thread).

## Role 2 in detail: third-party instruments

### Master or slave

The CORE is the **master** (Modbus TCP client). Practically every analyser with Modbus is a slave:
it holds its readings in registers and waits to be asked. Being master also means the CORE decides
how often each value is read, knows at once when an instrument stops answering, and never has to
accept connections from the instrument network.

### Which network

The instruments go on the **I/O network** (the W5500 port), with the I/O boards. Reasons:

- it is ours: a fixed private subnet we choose, isolated from the client's network by hardware (two
  separate chips, the I/O traffic never enters lwIP);
- the client port stays as it is: the CORE only answers there (role 3, HTTPS, SNTP, email), so
  nothing on the client's network can reach the analysers through the CORE;
- the analysers usually sit in the same cabinet or shelter as the I/O boards, on one small switch.

An instrument that only has RS-485 (Modbus RTU) goes through a Modbus TCP to RTU gateway on the
same network (Moxa MGate, Advantech, Lantronix and similar): the gateway's IP, and the instrument's
RTU address as the unit id. Several instruments behind one gateway share one connection.

### What is configurable

Everything is an ordinary parameter (IDs 500-621, `inst.h`), so the web page, the touchscreen's
settings list, backups and the configuration checksum all handle it with no new mechanism.

Up to **8 instruments**, each:

| Setting | Parameter | Default |
|---|---|---|
| enable | 500 `inst_enable[k]` | off |
| IP address | 501 `inst_ip[k]` | not set |
| TCP port | 502 `inst_port[k]` | 502 |
| unit id | 503 `inst_unit[k]` | 1 |
| poll period | 504 `inst_period_ms[k]` | 1000 ms (100 ms to 60 s) |
| reply and connect timeout | 505 `inst_timeout_ms[k]` | 1000 ms |
| most registers per request | 506 `inst_max_regs[k]` | 64 (1-125; some devices take fewer) |
| hole allowed inside one request | 507 `inst_gap[k]` | 0 (many devices refuse a read across unmapped registers) |
| status point, and its fault / maintenance / calibration bit masks | 508-511 | none |
| name | 521-528 | "" |

Up to **32 points** (values) in all, each:

| Setting | Parameter | Default |
|---|---|---|
| instrument (0 = not used) | 530 `pt_inst[p]` | 0 |
| function: 1 coils, 2 discrete inputs, 3 holding, 4 input registers | 531 `pt_fc[p]` | 3 |
| address, 0-based | 532 `pt_addr[p]` | 0 |
| type: u16, i16, u32, i32, f32, bit of a register | 533 `pt_type[p]` | u16 |
| byte order: ABCD (standard), CDAB (word swap), BADC (byte swap), DCBA (little-endian) | 534 `pt_order[p]` | ABCD |
| bit (for type bit) | 535 `pt_bit[p]` | 0 |
| scaling: value = raw x gain + offset | 536-537 | 1, 0 |
| valid range lo..hi | 538-539 | none |
| name, unit | 550-581, 590-621 | "" |

Read-only results: 540 `pt_value[p]` (float, NaN until read), 541 `pt_q[p]` (quality code), 542
`pt_raw[p]` (the bits as received, for fault finding), 515-520 per instrument (state, polls that
worked, failed requests, last error, last exception, response time).

### How it polls

Every poll period each instrument's points are read in as few requests as its settings allow:
points of the same function, sorted by address, are joined while the request stays within
`max_regs` and the hole between two points is no more than `gap`. One request is outstanding per
connection. Nothing waits: the poller starts a connection or a request and comes back for the
answer in the next I/O bus cycle (20 ms), so a missing analyser can never slow the I/O boards.
Three TCP connections are kept open at a time; with more devices than that, an idle connection is
handed over, which only costs a TCP handshake on a local network.

### Quality

Each point carries one of the platform's eight quality codes:

| Situation | Quality |
|---|---|
| not used, or its instrument off | unavailable (value NaN) |
| never read yet | unavailable |
| the last read failed (no reply, connection lost, gateway 0A/0B) | stale (last value kept) |
| 3 polls in a row failed: the instrument is "lost", retried every 5 s | unavailable |
| the instrument refused that read (exception 01-04: a wrong setting) | fault |
| the instrument's status point has a fault / calibration / maintenance bit set | fault / cal / maint |
| NaN, infinite, or outside lo..hi | out of range |
| otherwise | valid |

The status point is how an analyser's own state (in calibration, maintenance, fault: NAMUR NE 107
on most current analysers) reaches the CORE without any analyser-specific code.

### Finding the right data: the register explorer

Commissioning means hunting for the right register, type and word order. The web API reads any
range of registers from any address on the I/O network once and returns the raw bytes and words
(`POST /api/inst/explore`, `GET /api/inst/explore`, admin login; reads only). The web page then shows
the same words decoded every way (u16, i16, u32, i32, f32, in all four orders), so the technician
picks the line that matches the analyser's display. Once found, the point is saved with those
settings.

### Instrument profiles

A profile is a file of point settings for one analyser model (or for the VDI 4201-3 layout, which
covers every analyser that implements it): its registers, types, order, status
word and masks, imported on the web page and written as ordinary parameters. Profiles are only
written from the maker's Modbus manual for that model and firmware, and checked on the real
instrument with the explorer; none are shipped until a model has been checked.

### Where the values go

- **Web and touchscreen:** `/api/inst` (state per instrument, value and quality per point), and the
  parameters in `/api/values` like any other.
- **Calculations:** `inst_point(p, &value, &quality)` gives any task the value and quality as of the
  last scan. The emissions engine (PR #15) picks its inputs by measurement index (parameter 430);
  instrument points are to be added as a source there (asked of the emissions engine thread).
- **Client output (role 3):** through the calculations into the standard block, or a product's own
  map entry on parameter 540 (an array: one float per point).

### Scope: kept bounded

This is the acquisition side of a CEMS, not a general data acquisition system. In:

- reading values and status from up to 8 devices and 32 points, with quality;
- the register explorer for commissioning.

Out (each could be added later without changing the above):

- writing to instruments (starting an analyser's calibration, sending it a pressure): it needs
  interlocks and an audit trail, and a wrong write can upset a running analyser;
- instruments that push data (the CORE as slave on the I/O network);
- RS-485 on the CORE board itself (removed in the PCB decisions; a gateway covers it);
- logging every raw point separately (the data log takes the values the product logs);
- instruments on the client's network.

## Status of this change

Built and host-tested (`tests/test_inst.c`, `tests/integration/inst_test.py` against a simulated
analyser in the PC simulation): decoding in every type and order, request grouping, values,
scaling and every quality rule, a gateway with two units, five devices over three connections,
refused and missing devices, the explorer, the full path over the W5500 driver beside the I/O
boards. RAM: 8.1 KB static (fixed size), no new task. Not yet run on the DK. Still to do in other threads: the web page's instrument tab and
explorer (web thread), an instrument status page on the touchscreen (HMI thread), instrument points
as an emissions engine source (emissions engine thread).
