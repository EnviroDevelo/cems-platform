# Operator and service manual: the web page

This manual is for people who use the instrument from a browser: operators who watch values
and acknowledge alarms, administrators who set it up, and service staff. It describes the
platform's web page, which every product built on the CEMS core shares. A product's own values,
names and units are described in that product's manual.

Other manuals:
- The touchscreen: `docs/OPERATOR_MANUAL.md`. The web page uses the same words, marks and
  colours (`docs/UI_CONVENTIONS.md`).
- Modbus and integration: `docs/MODBUS.md`.
- Service and repair: `docs/SERVICE_PROCEDURES.md`.

Pictures of the page are in `docs/web_*.png`.

## 1. Opening the page

- Connect a computer to the plant network port (the client port, not the I/O board port) and
  open `https://<unit IP>/`. The IP is on the touchscreen System screen.
- The first time, the browser warns that the certificate is not trusted. Each unit makes its
  own certificate. Check that the fingerprint at the bottom of the page matches the touchscreen
  System screen, then continue. To stop the warning, use **Download the certificate** and
  install it as trusted on that computer.
- The page works in Edge, Chrome, Firefox and Safari, and on a phone (the tabs scroll sideways).
- Several people can have the page open at once.

**Connection lost.** If the unit stops answering, a red banner says "Connection lost" with the
time of the last update, and the values grey out. The page keeps trying and comes back by
itself when the unit answers again: no reload is needed.

## 2. The header

| Item | What it tells you |
|---|---|
| Product, serial, firmware | Which instrument and which firmware (with its build and the core version) |
| **State** | What the instrument is doing: the same states as the touchscreen (operator manual 2) |
| **LOCAL / REMOTE** | Who has control. In LOCAL most settings are changed at the touchscreen, and the page says which ones it can still change. REMOTE is set at the touchscreen only |
| **Link** | Whether the PLC or SCADA is polling (LINK OK, LINK LOST, NO LINK) |
| **Alarms** | How many alarms are active and how many are not acknowledged. Click it to open the alarm list |
| **live** | The page is getting fresh values |

## 3. Logging in

You can watch values, alarms, trends and the event log without logging in. To change anything,
log in with the level's password:

| Level | Can |
|---|---|
| operator | Acknowledge alarms, change operator settings |
| admin | Also passwords, clock, email, alarm actions, backup and restore, network and Modbus settings |
| factory | Also calibration, firmware updates, the service log, manufacturing tests and the board test |

- Each setting's row in Settings says which level it needs.
- Five wrong passwords in a row lock logins for 30 seconds.
- While a login is being checked the button says "Logging in…". "Wrong password" means the unit
  checked it; "No answer from the unit, check the connection" (after 10 seconds at most) means the
  password never reached it. While the page has no connection, a note by the login box says so.
- A login ends after 10 minutes with no change; the header shows the minutes left.
- **Closing the page logs out.** When every tab of this unit has been closed, the next page you
  open starts logged out. A reload, or a second tab next to one that is open, keeps the login.
- While the factory default passwords are in use, a warning links to Administration. Change
  them at commissioning (4 to 32 characters, no spaces).
- **Lost passwords?** (shown when logged out): the panel gives a reset code line. Send it to the
  manufacturer, who returns a one-time token that resets all passwords to the defaults.

## 4. The tabs

| Tab | What it holds |
|---|---|
| Overview | The product's own screens, live, and the alarm list |
| Trends | Live charts of up to 4 values |
| Settings | Every setting with its value, range and who may change it |
| Calibration | Only on products with calibrated channels |
| Service | Hours, self-test, service log, manufacturing test, board test, system health, Modbus |
| I/O boards | Only when an I/O board is enabled |
| Instruments | Analysers and other devices read over Modbus TCP (shown once one is set up; always at admin login) |
| Event log | The newest events and the downloads |
| Administration | admin login: passwords, clock, backup, firmware, email |
| Alarm setup | admin login: what each alarm does |

The address keeps the open tab (for example `#service`), so a reload or a bookmark opens the
same tab. A tab you can't see yet (Administration while logged out) opens once you log in.

## 5. Overview and alarms

- Values update about twice a second. A value that isn't valid is greyed with the same word as
  on the touchscreen: stale, unavailable ("----"), out of range, warming up, maintenance,
  calibration or fault. See operator manual 3 for what each means and what to do.
- Each alarm row shows its severity, name, code, when it started, whether it is still active and
  whether it has been acknowledged, and a "What to check" line.
- **Acknowledge** (operator login) acknowledges one row; **Acknowledge all** does every row. A
  latched alarm stays in the list until it is acknowledged, even after its cause has cleared.

## 6. Trends

- Pick up to 4 values. Each gets its own scale; click a value's name to show its scale.
- Hover over the chart to read the values at that time. **Hold** freezes the chart.
- **Save CSV** downloads the points recorded while the page was open. The chart is drawn from
  the page's own readings: it starts when you open the tab.
- The chosen values are remembered in that browser.

## 7. Settings

- Type in the filter to find a setting by name or number.
- **Change** appears on rows your login and the LOCAL/REMOTE mode allow. A value outside the
  allowed range is refused and the page says why.
- A change shows within a second and appears on the touchscreen and over Modbus. Every change is
  in the event log with the old and new value and who made it.
- Settings are kept through a power cut.

## 8. Calibration (factory login, REMOTE)

**Begin** starts a calibration of a channel: the header shows CALIBRATION and the values carry
"calibration". **Accept** puts the new coefficients in force and adds them to the history;
**Abort** keeps the old ones. **Revert to factory** puts the factory record back. The table
shows the record in force, the factory record and when the next calibration is due.

## 9. Service

- **Hours**: operating hours and hours since the last service, and the settings checksum
  (compare it with the touchscreen System screen).
- **Self-test**: one badge per start-up check. A failure also raises an alarm.
- **Service log** (factory login to add): who did what and when. It is kept in factory memory and
  survives firmware updates, restoring defaults and restoring a backup.
- **Manufacturing test**: the last recorded test. PASSED (green), FAILED (red), or NOT PASSED
  (amber) when nothing failed but some tests were skipped or not run.
- **Board test** (factory login): **Run board test** checks the board's parts in about 3 seconds
  and saves the result as the manufacturing test record. See `docs/SERVICE_PROCEDURES.md` 4.
- **System health**: memory free and its lowest, each task's unused stack (red below 512 bytes),
  logic scan time and overruns, secure connection counts, and the last fault with its registers.
  Service staff use these to diagnose a unit; send them with any fault report.
- **Modbus TCP**: whether the server is listening, control systems connected, requests and when
  the last one came, refused requests, whether Modbus writes are allowed and the allowed client
  address. **Modbus settings** opens the Modbus settings. See `docs/MODBUS.md` 7 and 8.
- **Force Modbus values** (admin login, for commissioning): pick a value, enter what the control
  system should read, and **Force for 30 s**. The control system reads the forced value for 30
  seconds, then the real value again; forcing again restarts the 30 seconds. Test bits have one
  button each: click for forced on, again for forced off, again to release. **Release** or
  **Clear all** ends forcing early. This page, the touchscreen, alarms and the logs always keep the
  real value, and the table shows both. While anything is forced, the header shows an amber
  **MODBUS FORCED** on every tab, and the PLC can see it in register 9090 and the common alarm.
  Every force is in the event log.
- **Modbus handover file (Excel)** (any login): downloads this unit's register map as an Excel
  workbook for the control system integrator: a cover with the connection details and a
  sign-off box, the full map with a "Checked by integrator" column, the platform block, the codes
  (alarm IDs, exceptions) and the values that can be forced. **CSV** gives the map alone.

## 10. Event log and downloads

- The newest 20 events: power-ups, logins, setting changes, alarms, firmware changes.
- **Events CSV** and **Data CSV** download the logs for Excel. A power cut shows in the data as
  one GAP row.
- **Diagnostic package** downloads one file with everything service needs (settings, alarms,
  events, firmware, health). It contains no passwords. Send it with a fault report.

## 11. Instruments (analysers on the I/O network)

The instrument can read up to 8 other devices, such as gas analysers, over Modbus TCP on its I/O
network port, and up to 32 values from them in all. It asks; they answer. A device with only
RS-485 goes through a Modbus TCP gateway: give the gateway's IP and the device's RS-485 address
as the unit ID.

- **The list** shows each device's state (ok, connecting, failing, lost, bad settings), how many
  reads worked and failed, the last error and the reply time, and a tile per value with the same
  quality words as everywhere else. When the device's status word says fault, maintenance or
  calibration, its values carry that word.
- **Set up / Edit** (admin login, REMOTE): IP, port, unit ID, how often to read, the timeout, the
  most registers per read and the gap allowed inside one read (leave 0 unless the device's manual
  says it accepts reads across unused registers), the status point and which of its bits mean
  fault, maintenance and calibration. Below, one row per value: name, unit, function (03 holding,
  04 input, 01 coils, 02 discrete inputs), address (0-based, as on the wire: Modicon 40001 is
  holding 0), type, byte order, bit, gain and offset (value = raw × gain + offset), and the valid
  range. **Save** writes them; the device is read again at once.
- **Flags** (a point's column): for analysers that report each warning as its own bit (a discrete
  input) instead of one status word, set the bit's point to **fault**, **calibration** or
  **maintenance**. While that point reads non-zero, every other value of the instrument carries
  that word.
- **Register explorer** (admin): reads registers from any address on the I/O network without
  writing anything. Each row shows the words decoded every way: unsigned and signed 16-bit (and
  byte-swapped), and 32-bit float, unsigned and signed in all four byte orders (ABCD is the Modbus
  standard; CDAB is the common "word swapped" order). Type the number the device shows on its own
  display into **Find value**: the matching readings turn green. Click the right one and **Use this
  for a new point** fills a row in the editor.
- **Profiles**: **Export profile** saves a device's settings and points as a JSON file, without its
  IP. **Import profile** loads one into the instrument being edited, so the same analyser model is
  set up in one step on the next site.

## 12. Administration (admin login)

- **Passwords** for each level.
- **Clock**: set it to the computer's time, or give an SNTP time server. The panel says where
  the time last came from.
- **Backup and restore**: download the settings as a file; restoring puts them back. The serial
  number, calibration and service log are never changed by a restore.
- **Firmware** (factory login): upload a `.cfw` file, then restart on it. The new firmware runs
  on trial and keeps itself only if it starts properly; otherwise the unit goes back to the old
  one. The panel shows the version running, the one waiting in the spare bank, the bootloader's
  check of the image, and the update history. See `docs/SERVICE_PROCEDURES.md` 1.
- **Email**: mail server, sender, recipients. **Send test email** checks the settings.
- **I/O boards**: find the boards on the I/O port and give a new board its address.

## 13. Alarm setup (admin login)

For each alarm, tick what it does: **Email** sends a message, **Modbus** counts it in the
common-alarm register, **Relay 1-4** switches a relay output. No tick: the alarm is only shown.
**Default** puts the row back. The relay mode (fail-safe or not) is set here too.

**Digital input alarms** (24 slots, alarms 30 to 53): tie a digital input to an alarm. For each
slot pick the input, whether the alarm is raised when the input is **on** or **off**, the delay in
seconds before it is raised (0 to 3600), whether it latches until acknowledged, its severity, and
a label such as "Door open". **Save** writes only what you changed. The label names the alarm
everywhere: the alarm list, the touchscreen, the Modbus map (discrete input 7000 + slot) and the
Modbus handover file. The **Now** column shows each slot's state.

## 14. If something is wrong

| What you see | What to do |
|---|---|
| The browser can't reach the page | Check the IP on the touchscreen System screen, and that the computer is on the plant network |
| Certificate warning every time | Download the certificate and install it as trusted (1) |
| "Connection lost" stays | Check the cable and the network; the touchscreen still works. The page recovers by itself |
| "Not allowed in the current local/remote mode" | Set REMOTE at the touchscreen, or make the change there |
| "Your login level is too low" | Log in at the level the row's Access column names |
| Logged out unexpectedly | The login ends after 10 minutes with no change, or when every tab of the unit was closed |
| Passwords lost | Lost passwords? panel (3) |
