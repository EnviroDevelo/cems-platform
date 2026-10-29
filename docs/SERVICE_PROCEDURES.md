# Service procedures: update, recovery, provisioning, fault finding

For technicians and the factory. CIP-03.1 (documented update method), CIP-04.4 (recovery when
normal communications fail), CIP-23.2 (loading firmware and defaults) and CIP-23.3 (board test).
The developer's detail is in PLATFORM.md 5b, BOOTLOADER.md and CUBEMX_SETUP.md 3d.

Logins: operator, admin and factory. The factory (tech) login is needed for firmware, the
serial number, the board test and the test record.

## 1. Updating the firmware over the network

You need a laptop on the unit's plant network, the unit's address, the factory password, and the
update file (`cems-<version>.cfw`). On a production unit the file must be signed with the
product key; the Firmware panel says "signing required" when it is.

1. Make the unit safe to restart. An update restarts it: about 10 s without measurement and
   without Modbus. If the product has a safe-state rule (a calibration running, a process
   step), the restart is refused with the reason, and nothing is lost.
2. Open `https://<unit address>/` and log in as **factory**.
3. **Administration → Firmware → Choose file**, pick the .cfw, then **Upload**. The unit checks
   the file as it arrives: product, board revision range, minimum version, size, SHA-256 and the
   signature. A refused file shows why and the running firmware is not touched.
4. Press **Restart on <version>**. If "confirm on the touchscreen" is set (parameter 87), someone
   at the unit must press **Yes** within 5 minutes.
5. The unit restarts on the new version and runs it on **trial** for 60 s. It confirms itself
   when the scan runs and the display works for those 60 s. The Firmware panel then shows the
   new version as running, with nothing on trial.
6. If the new version does not confirm, it goes back to the previous one by itself. That happens
   after a watchdog reset, or after 3 starts behind the bootloader. The panel says "rolled back",
   and the event log and update history have an entry. The new file is still staged, so you can
   try it again on purpose.

Check afterwards: the version in the header, no new alarms, and the event log entry "firmware
updated". The previous version stays in the other bank until the next upload, as the way back.

## 2. Recovery when the network does not work

In order, least intrusive first:

| Symptom | What to do |
|---|---|
| Web page not reachable, unit running (display works) | Check the cable and the link LED. The touchscreen's **System** screen shows the unit's network address and link. Try `https://<that address>/` from a laptop on the same subnet |
| Forgotten passwords | Web login row → **Lost passwords?**: send the line it shows to the developer and enter the token you get back. All three passwords go back to the defaults (PLATFORM.md, CIP-19.3) |
| New firmware does not start | Wait: the watchdog and, behind the bootloader, the trial counter go back to the previous version within about 30 s |
| Both firmware copies bad, or the bootloader stopped (blank display, no network) | Open the housing. Connect an ST-LINK to **J5** (SWD) and program the factory bank image (BOOTLOADER.md 4) with STM32CubeProgrammer. The settings in FRAM are kept |
| No ST-LINK available | Fit the **JP1 BOOT0** jumper, power-cycle, and load the image over the ST ROM bootloader on the service UART **J6** with STM32CubeProgrammer (UART mode). Remove the jumper and power-cycle afterwards |

SWD and BOOT0 work only while the product state allows debug. That is OPEN on development units;
the production setting is still to be decided (CORE_PCB.md, CIP-20.4).

## 3. Factory provisioning, one new board

1. **Program.** STM32CubeProgrammer over J5: the bank image `bank.bin` at 0x08000000 and
   `cems_boot.bin` at 0x08100000 (BOOTLOADER.md 4). Then set write protection on the bootloader
   (BOOTLOADER.md 5). The unit starts; on its first start it makes its own HTTPS key.
2. **Network.** Connect the plant port and log in as factory from the factory PC (default
   address and passwords: PLATFORM.md).
3. **Serial number.** Administration → Serial number. Get a token from the developer for this
   chip ID and enter it (PLATFORM.md D25).
4. **Board revision.** Set parameter 51 `hw_rev` (factory) to the board's revision. On boards with
   straps, parameter 319 shows what the straps read; the two must agree.
5. **Board test.** Service → **Run board test** (CIP-23.3, below). Every line must pass; skipped
   lines are only acceptable where the product has no such part.
6. **Calibration.** The product's calibration procedure (Calibration tab). Until a factory value
   is set the unit shows CALIBRATION INVALID.
7. **Record.** The board test saved itself as the test record (station "board"). Add a service
   log entry "provisioned" with your name.
8. **Passwords.** Set the operator and admin passwords for the customer, or leave the defaults
   with a note for commissioning.
9. **Backup.** Administration → **Download parameter backup**, and file it under the serial number.

## 4. The board test

Service tab at factory login, or `POST /api/service/boardtest`. It takes about 3 s and checks:
the settings memory through every address line and chip (each byte put back at once), 4 KB of
RAM, the clock running, the firmware image against its header, the log flash, both Ethernet
chips and their links, the display, the 24 V supply (20.0 to 28.8 V), and the board revision
straps. Results are shown per line with the measured value. The whole set is saved as the unit's
test record.

It writes to the settings memory for a few milliseconds. Do not run it on a unit in service: a
power cut at that moment could damage a stored byte, which the settings' second copy would then
have to cover.

## 5. Fault finding

Start with what the unit already says about itself, in this order:

1. **The alarms** (touchscreen header, web Overview, Modbus 9005/9006). Each alarm's text says
   what to check; the full list with causes is in [FAULT_CODES.md](FAULT_CODES.md).
2. **The instrument state** (touchscreen header, `/api/info` `state`, Modbus 9003). FAULT and
   SAFE MODE always come with an alarm that says why.
3. **The last fault before a restart** (touchscreen System screen, `/api/info` `last_fault`).
   Codes are in FAULT_CODES.md, "Fault record". The reset cause is on the `power_up` event.
4. **The start-up self-test** (web Service section, `/api/info` `selftest`): one line per check.
5. **The event log** (web Event log tab, CSV download): every change, login, alarm, restart and
   update, with who did it (touchscreen, web, modbus).
6. **The diagnostic package** (web Event log tab, **Diagnostic package**): information,
   settings without passwords, alarms, firmware, I/O boards and the event log in one file.
   Send that file with any report to the manufacturer.

| Symptom | Likely cause | What to do |
|---|---|---|
| Display blank, web page works | display cable, or the display's supply | Alarm 8 `DISPLAY_FAULT` confirms it. Reseat the ribbon cable with the power off. The unit keeps measuring |
| Display and web both dead | no supply, or no firmware running (section 2) | Check 24 V at the terminals. Then section 2 |
| SAFE MODE after power-up | it restarted repeatedly within a short time | Read the last fault (above), write it down, power-cycle. If it comes back, send the diagnostic package |
| `UNEXPECTED_RESET` now and then | watchdog or supply dips | The last fault names the task. Compare the `power_up` reset cause: 6 brown-out points at the supply |
| `PARAM_WRITE_FAIL` | settings memory (FRAM) not answering or failing writes | Changes may be lost at power-off. Run the board test (section 4) at a quiet time; replace the core board if the FRAM line fails |
| `IP_CONFLICT`, network off | another device has the unit's static IP | Change one of the two addresses |
| PLC reads nothing | wrong IP or port, a client filter, or more than 4 clients | [MODBUS.md](MODBUS.md) 1 and 9. The web Service tab lists the Modbus clients and refused connections |
| PLC writes refused | LOCAL mode, Modbus writes off, or the value's level | The exception code says which: MODBUS.md 5 |
| `REMOTE_LINK_LOST` | in REMOTE and the PLC stopped polling | Check the PLC and the network. The timeout is parameter 3 |
| `IOB_LOST` | I/O board power, cable or address | Check the I/O board's power LED, the I/O network cable and its address switch; the web I/O boards tab shows each board's link |
| Clock wrong or "not synchronised" | no time server reachable | Check the SNTP server setting and the gateway, or set the clock by hand on the touchscreen or web |
| `RESOURCE_LOW` | free memory below its margin | Note the figures on the System screen or `/api/info` `health`, and send the diagnostic package. Not urgent, but don't ignore it |

Keeping the settings: web Administration tab → **Download parameter backup** beforehand, and
**Restore from file** on the same unit or its replacement. The calibration (factory data) is kept separately and isn't
changed by a restore of the defaults.

