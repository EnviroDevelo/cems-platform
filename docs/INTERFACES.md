# Interfaces: adding one, and keeping them compatible

For developers. CIP-13.5 is the written framework for adding an interface; CIP-14.5 is the
compatibility policy. The existing interfaces are the touchscreen (`platform/ui/`), Modbus
TCP/RTU (`platform/net/mbtcp.c`, `platform/core/modbus.c`, `mb_rtu.c`) and the HTTPS JSON API
(`platform/net/http_api.c`). Follow the way they do it.

## 1. Adding an interface (CIP-13.5)

An interface is a transport plus a mapping. It never owns data: it reads the published snapshot
and asks the parameter table for every change.

### Where it plugs in

| Piece | Where | Example |
|---|---|---|
| The task that runs it | `platform/os/plat_net_tasks.c` (network), or a task of its own in `plat_tasks_freertos.c`: an entry in `plat_tasks.h` (`PLAT_TASK_*`), a generous stack, and a watchdog check-in (`plat_checkin`) | Modbus TCP has `PLAT_TASK_MODBUS` |
| The socket / UART | `plat_net.h` sockets (lwIP on the target, POSIX in netsim), or an `iob_port_t` for serial | `mbtcp.c` |
| The mapping | a table built from `param_def_t`: never a second copy of names, ranges or units | `mb_map_t` for Modbus, the `/api/params` walk for the web |
| A source ID | `plat_src_t` in `plat_cmdq.h` (add one, after `PLAT_SRC_MODBUS`) | `PLAT_SRC_WEB` |
| Tests | a host test in `tests/`, and a netsim integration test in `tests/integration/` | `test_modbus.c`, `net_test.py` |

### Rules it must follow

1. **Read** with `param_read()` (one value from the end of the last scan) or under the snapshot
   lock. Never read an application variable directly from another task.
2. **Write** only with `param_post()` / `param_blob_post()` / `param_record_post()`, giving its
   source and the requester's login level. These run the same range, level, mode (LOCAL/REMOTE)
   and interlock checks as every other interface. Return the `param_err_t` to the client in the
   interface's own way: a Modbus exception, an HTTP status and JSON error.
3. **Respect the flags.** `PF_RO` never written. `PF_SECRET` never read back or exported.
   `PF_NO_MODBUS` means a network or security setting: keep it off any interface that has no
   login. `PF_CONFIRM` needs an explicit confirmation step where the interface has a person
   in front of it.
4. **Authenticate** before any write. Map the interface's login onto `plat_level_t`. An interface
   with no login writes at one fixed level, never above operator. Modbus does this: operator,
   REMOTE mode only, writes switchable off (`modbus_write_en`), and an allowed-client IP filter.
5. **Report activity** with `param_comms_activity(src, now)` on every valid request, so "remote
   link lost" (`param_remote_link_lost`) covers it.
6. **Carry quality and state.** Every value goes out with its quality (`param_quality`, names
   from `plat_quality_name`), and the instrument state (`plat_state`, CIP-09) is readable.
   A value that is not valid must never look valid on the wire.
7. **Bound everything.** Fixed buffers, a request size limit, a timeout per client, a limit on
   clients, and no allocation outside the fixed pools (HTTPS uses the mbedTLS pool). A flood
   of requests must slow the interface, never the scan.
8. **Log** what changes the unit: settings changes are logged by the parameter table already.
   Log logins and refusals the way `web_auth.c` does.
9. **Version** its mapping (section 2) and add it to the interface snapshot (`make snapshots`).

### Checklist for the pull request

- [ ] No new copy of a name, unit or range: all from `param_def_t`.
- [ ] Every write goes through `param_post*` with a new `PLAT_SRC_*`.
- [ ] Unauthenticated means read-only.
- [ ] Quality and state carried.
- [ ] Bounded memory and time; a host test for oversized and malformed requests.
- [ ] Snapshot added and `make test` passes (it runs `snapshots-check`).
- [ ] PLATFORM.md section and the task's stack added to MEMORY.md.

## 2. Compatibility policy (CIP-14.5)

What a SCADA system, a script or a saved browser bookmark can rely on.

### Version numbers

- **Core version** (`plat_version.h`, MAJOR.MINOR.PATCH): the rules there. PATCH fixes only;
  MINOR adds; MAJOR may break, with a migration note in CHANGELOG.md.
- **Modbus map version** (input register 9001) and **platform block layout** (register 9000 block,
  layout 2 today).
- **Web API version** (`/api/info` `"api"`, 2 today).
- **Settings layout version** (the FRAM image, `--layout` in the update file).

### The rules

1. **Within one major version an address or field never moves or changes meaning.** A Modbus
   register keeps its address, type, scaling and word order. A JSON field keeps its name, type
   and units.
2. **New things are only added.** New registers go into free addresses, new JSON fields into
   existing objects, new endpoints beside the old ones. Clients must ignore what they don't
   know. This is a MINOR change.
3. **Removing something takes one major version of notice.** A removed Modbus value keeps its
   address and reads as "unavailable" (quality code, value 0) for the rest of that major
   version. A removed JSON field becomes `null` the same way. It goes away at the next MAJOR.
4. **Settings move forward.** New firmware loads the settings of older firmware (records by ID,
   the `migrate` hook for a renumbered or rescaled one, PLATFORM.md section 4). Older firmware loading
   newer settings keeps what it knows and flags `PSTAT_NEWER_LAYOUT`.
5. **Platform IDs 1-999 mean the same in every product**; product IDs start at 1000 (CIP-14.6).
6. **The snapshots enforce it.** `docs/interface/<app>/MODBUS_MAP.md` and `API.txt` are generated
   from the tables (`make snapshots`). `make test` fails when the Modbus map differs from the
   snapshot. A difference that only adds is a MINOR bump plus `make snapshots`. A difference
   that moves or removes is a MAJOR bump, or it is a mistake.

### What a client should do

- Read the versions first (register 9001, `/api/info` `"api"`), and refuse a MAJOR it doesn't know.
- Ignore unknown registers and fields.
- Check the quality of every value, not just its number.
