# Operator manual: the touchscreen

This manual is for people who run the instrument from its 7" touchscreen. It describes the platform's screens, which every product built on the CEMS core shares. A
product's own measurement screens, names and units are described in that product's manual.

Other manuals:
- Modbus and integration: `docs/MODBUS.md`.
- Service and repair: `docs/SERVICE_PROCEDURES.md`.
- The web page shows the same information, using the same words and colours (`docs/UI_CONVENTIONS.md`).

Pictures of every screen are in `docs/ui/`.

## 1. Using the screen

- **Tap and release.** An action happens when you lift your finger from the button. If you slide off the button before lifting, nothing happens.
- **Asking first.** Some actions ask "Are you sure?" before they take effect: changing control between LOCAL and REMOTE, entering or leaving maintenance, calibration steps, network changes, a firmware restart, and some settings.
- **Dimming.** The screen dims after 10 minutes with no touch. The first touch only wakes it, so it can't press anything by accident.
- **Greyed buttons.** A greyed button can't be used right now. Tapping it, or the line under it, tells you why.

## 2. The header (top of every screen)

From left to right:

| Item | What it tells you |
|---|---|
| Product name | Which instrument this is |
| **State** | What the instrument is doing (table below) |
| **LOCAL / REMOTE** | Who has control. LOCAL: settings are changed here. REMOTE (green): the PLC or the web page has control, and changes made here are refused |
| **Link** | The supervisory system. LINK OK (green): the PLC or SCADA is polling. LINK LOST (red): polling has stopped for longer than the configured timeout. NO LINK: nothing has polled since start-up, or the timeout is off |
| **Alarms** | Red: an alarm or fault is not acknowledged. Amber: a warning is not acknowledged. Grey: all alarms are acknowledged, and the pill still shows how many are active. Tap it to open the alarm list |
| Time | Local standard time. "--:--:--" means the clock is not set (see 8) |

| State | Meaning | Colour |
|---|---|---|
| STARTING, INITIALISING | Starting up | Neutral |
| WARMING UP | Not yet ready to measure | Amber |
| READY | Ready, not measuring | Green |
| MEASURING | Measuring normally | Green |
| MAINTENANCE | Someone has put it in maintenance (see 5) | Amber |
| CALIBRATION | A calibration is under way (see 6) | Amber |
| FAULT | A fault stops measurement. The alarm list says what it is | Red |
| SAFE MODE | It restarted repeatedly after crashes and is running without its measurement logic. Call service | Red |

## 3. Values and what the marks mean

A normal reading is dark text with its unit. Any other reading is greyed, and a word under it says why:

| Word | Meaning | What to do |
|---|---|---|
| stale | The value has not been updated recently | Check the alarm list, then the I/O boards screen |
| unavailable (shown as "----") | There is no value | Check the alarm list |
| out of range | The reading is outside the instrument's range | Check the process |
| warming up | Not valid until warm-up ends | Wait |
| maintenance | The instrument is in maintenance | End maintenance when the work is done |
| calibration | A calibration is under way | Finish or abort the calibration |
| fault | The measurement has a fault | The alarm list and the I/O boards screen give the reason |

In a grid, such as a set of valves or channels, each cell carries its own mark.

## 4. Logging in

Viewing needs no login. Changing things does:

| Level | Can |
|---|---|
| operator | Everyday operation: hand control to REMOTE, maintenance, and settings marked for operators |
| admin | Commissioning: most settings, the clock, alarm actions, calibration (by default) |
| factory | Service: firmware, factory settings and factory calibration |

1. Tap **Log in** (bottom right).
2. Choose the level.
3. Type the password and tap **Enter**.

After five wrong passwords the login locks for 30 seconds. A login ends after 10 minutes with no touch, or when you tap **Log out**.

The passwords are set at commissioning. If a password is lost, see the service manual.

## 5. Home

Home shows the main readings, plus two controls:

- **Hand to REMOTE / Take LOCAL control.** Handing control to the PLC and web needs an operator login. Anyone can take LOCAL control back at the instrument, because the person standing at it always wins.
- **Maintenance / End maintenance.** This needs an operator login. In maintenance the state shows MAINTENANCE and readings are marked "maintenance", so the PLC knows not to trust them. Always end maintenance when the work is done.

## 6. Calibration (System, then Calibrate)

The **Calibrate** button reads **Cal DUE** in amber when a channel is due.

- **Channels** lists each channel's status: "calibrating now", "never calibrated", "DUE since ...", "due ...", or the last calibration's kind and date. Tap a channel to select it.
- For the selected channel the screen shows the calibration in force, the factory calibration, and the points of the last calibration: reference, as found, as left.
- **History** shows the latest calibrations and their outcome: pass (green), fail (red), or other outcomes (amber).

To calibrate:
1. Log in. The default is admin, and your instrument may require another level.
2. Select the channel and tap **Start calibration**.
3. Follow the product's calibration procedure.
4. Tap **Accept** to keep the result, or **Abort** to keep the previous calibration.

**Revert to factory** puts the factory calibration back.

If a step is refused, the reason shows under the button. For example, you may need to log in, or the instrument may be in a state that doesn't allow calibration.

## 7. Alarms

Open the alarm list by tapping the alarm pill. Each row shows:
- the alarm;
- when it started, or how long after start-up if the clock was not set;
- whether it is still active.

A coloured bar on the left gives the severity.

- **Acknowledge** acknowledges one alarm. **Ack all** acknowledges every alarm. Neither needs a login.
- An alarm that has cleared stays in the list until it is acknowledged. A latched alarm stays active until it is acknowledged.
- **Tap an alarm** to see "What to check" for it. Tap it again to close that.
- **Actions** (admin) sets what each alarm does: email, Modbus, and relays 1 to 4.

The full list of alarm and fault codes is in `docs/FAULT_CODES.md`.

## 8. Settings, Events and System

- **Settings** lists the instrument's settings by page. Tap a row to change it. Some settings, such as long lists and byte values, can only be changed on the web page, and the row says so. In REMOTE, changes here are refused.
- **Events** is the event log: settings changed, logins, alarms, power-ups, firmware changes. Each entry shows who made the change and from where (touchscreen, web, Modbus).
- **System** shows:
  - firmware: the version, core version, build, update state and self-test result;
  - the unit: serial number, hardware, operating hours, supply voltage and restarts;
  - network: the address;
  - storage: calibration, log, configuration checksum and clock.

  System also holds these controls:
  - **Set clock** (admin). Only needed when there is no time server. "Clock, not synced" means nothing outside has confirmed the time;
  - **I/O boards**: each board's link, inputs, outputs, temperatures and faults, in words;
  - screen brightness;
  - **Display test** (see 9).

  The HTTPS fingerprint at the bottom lets a technician check that their browser has reached this unit.

## 9. Display test (System, then Display test)

This page is for checking the screen itself. Nothing on it changes the instrument.

- The status page shows what the display controller reports.
- **Colour test** steps through full-screen colours and a grid; tap anywhere for the next. Use it to look for dead pixels or a shifted picture.
- **Touch test** shows five crosses. Touch each one, and the screen tells you how far from the cross the touch landed. More than 10 px off shows red: call service. Tap **Done** to return.

## 10. If something is wrong

| You see | Do |
|---|---|
| Red alarm pill | Open the list, tap the alarm, follow "What to check" |
| State FAULT or SAFE MODE | Open the alarm list; call service with the alarm text and the System page details |
| LINK LOST | Check the PLC/SCADA connection and the network |
| Values "stale" or "unavailable" | Open I/O boards: a board marked lost or faulty is the usual cause |
| The screen is dark but the instrument runs | Touch it (it may be dimmed). If it stays dark, the instrument keeps measuring; use the web page and call service |
| Touches land in the wrong place | Run the touch test (9) and report the offsets |
