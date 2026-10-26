# <Product name>: product profile

Copy this file to `app_<product>/PROFILE.md` and fill it in (Common Instrument Platform
Specification section 9.2). It is the only place a product records what it fits and any
exception to a mandatory (M) requirement. `make conformance APP=app_<product>` refuses a
product without one, or with a section left as a placeholder.

| | |
|---|---|
| Product | <name, as `plat_app_t.ident.product`> |
| Product version | <MAJOR.MINOR.PATCH, as `ident.revision`> |
| Core version built on | <core-MAJOR.MINOR.PATCH tag; `/api/info` "core"> |
| Core board revisions supported | <e.g. A-B, as the image header's hardware range> |
| Owner | <who answers for the product> |

## Optional items fitted

One line per O (optional) requirement: fitted or not, and the setting that goes with it.

| Requirement | Fitted? | Notes |
|---|---|---|
| CIP-02.6 Clock quality exposed | <yes/no> | |
| CIP-05.4 Configuration checksum | <yes/no> | |
| CIP-12.5 Configurable alarm actions (email, Modbus, relays) | <yes/no> | <which relays> |
| CIP-13.4 Physical interfaces | <plant Ethernet, I/O Ethernet, RS-485 ...> | |
| CIP-16.3 Log retention | <log flash size, channels, interval: days kept> | |
| CIP-17.7 Operator identity | <login level only / named users> | |
| CIP-18.7 Calibration procedure | <described in ...> | |
| CIP-20.7 Secure boot | <yes/no> | |
| CIP-21.8 Local screen | <touchscreen / web only> | |
| CIP-24.5 Enclosure protection | <IP rating> | |

## Measurement channels

One line per entry in `plat_app_t.meas` (quality comes from the platform; say where each
value comes from and when it is stale).

| Index | Parameter | Name | Unit | Source | Stale after | Notes |
|---|---|---|---|---|---|---|
| 0 | <id> | <name> | <unit> | <input / calculation> | <ms> | |

## States

When the product reports each of the states the platform does not decide itself (section 7):
WARMING_UP, READY, MEASURING, and what MAINTENANCE does to its outputs.

| State | When | Outputs |
|---|---|---|
| WARMING_UP | <condition> | |
| READY | <condition> | |
| MEASURING | <condition> | |
| MAINTENANCE | operator selects it | <what the outputs do> |

## Product alarms and start-up checks

Listed in the generated `FAULT_CODES.md` (`make fault-codes APP=...`); each alarm needs a `.help`.

## Exceptions to mandatory requirements

None, or one line each with the reason and Gary's sign-off. CIP-02.4 (local standard time,
decided 26 Oct) is the platform's own and needs no line here.

| Requirement | Exception | Reason | Signed off (who, date) |
|---|---|---|---|
| - | - | - | - |
