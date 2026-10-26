# UI conventions (CIP-21.7)

One page that every product's screens follow, on the touchscreen (`platform/ui/`) and the web
page (`web/index.html`). Product screens described through `ui_app.h` get these rules
automatically. A product's own wording goes in its parameter table, not in new styles.
Changing a rule here is a core change (`INSTRUMENT_PLATFORM_SPEC.md` 9.4).

## 1. Colours

One palette for both interfaces. The names are the touchscreen's (`ui.c`), and the web
page's CSS variables carry the same values.

| Token | Value | Web | Used for |
|---|---|---|---|
| C_HEAD | #14213D | header | Header bar, the selected tab or level |
| C_BG | #F4F5F7 | `--bg` | Page background |
| C_CARD | #FFFFFF | `--card` | Cards |
| C_INK | #1D2330 | `--ink` | Text, valid values |
| C_MUTE | #667085 | `--mute` | Labels, notes, limits |
| C_LINE | #D9DDE4 | `--line` | Borders, idle indicators |
| C_ACCENT | #1F5FAE | `--accent` | Buttons, inputs that are on |
| C_PRESS | #0E3A73 | | A button while pressed |
| C_OK | #16794A | `--ok` | Healthy, on, saved, REMOTE |
| C_WARN | #A15C00 | `--warn` | Warning, attention, a temporary condition |
| C_BAD | #B42318 | `--bad` | Alarm, fault, refused, lost |
| C_GREY | #8A94A6 | | Disabled, not fitted, a status-only alarm |
| (neutral) | #5B6577 | | LOCAL, Cancel, acknowledged alarms |

Colour never carries meaning on its own. Every coloured mark also has a word ("online",
"fault", "LINK LOST") or a number, so it still reads for a colour-blind operator or in
direct sun.

## 2. Severity

| Alarm severity (`plat_alarm.h`) | Colour | Header pill |
|---|---|---|
| AL_SEV_FAULT, AL_SEV_ALARM | C_BAD | Red while any of them is unacknowledged |
| AL_SEV_WARNING | C_WARN | Amber while unacknowledged |
| AL_SEV_STATUS | C_GREY | Neutral |

Once everything is acknowledged, the pill turns neutral and still shows the count of active
alarms. On the web, an unacknowledged row is bold.

## 3. The header (every screen)

From left to right: the product name, the instrument state, LOCAL/REMOTE, the supervisory
link, the alarm pill (tap it for the list), and the time ("--:--:--" when the clock is not set).

| Element | Words | Colour |
|---|---|---|
| Instrument state (CIP-09, 21.2) | The state name in capitals: STARTING, INITIALISING, WARMING UP, READY, MEASURING, MAINTENANCE, CALIBRATION, FAULT, SAFE MODE | MEASURING and READY: C_OK. WARMING UP, MAINTENANCE and CALIBRATION: C_WARN. FAULT and SAFE MODE: C_BAD. STARTING and INITIALISING: neutral |
| Control | LOCAL / REMOTE | REMOTE: C_OK. LOCAL: neutral |
| Supervisory link (CIP-21.5) | LINK OK / LINK LOST / NO LINK | C_OK / C_BAD / neutral. LOST means no request for `comms_timeout`. NO LINK means none since start, or the timeout is 0 |

The web page shows the same elements with the same words. While the page itself cannot reach
the instrument, it adds its red "Connection lost" banner and greys every value.

## 4. Values and their quality (CIP-10, 21.4)

- A value is shown with its unit, after a space ("21.4 °C", "12 mg/m³"). The unit and the
  decimal places come from the parameter table. The own fonts (D24) carry ° ² ³ µ and the
  other symbols, and nothing else is spelled out ("deg C" is not used).
- A **valid** value is drawn in C_INK with no mark.
- Any other quality keeps the last number visible but greys it (C_GREY) and puts the quality
  word next to it, in lower case:

| Quality | Word | Word colour |
|---|---|---|
| valid | (none) | |
| stale | stale | C_WARN |
| unavailable | unavailable (no number shown: "----") | C_GREY |
| out-of-range | out of range | C_BAD |
| warming-up | warming up | C_WARN |
| maintenance | maintenance | C_WARN |
| calibration | calibration | C_WARN |
| fault | fault | C_BAD |

- No screen shows a non-valid reading in the valid style, including tiles, grids and the I/O
  boards screen. Where there is no room for the word, the tile's border takes the word's colour
  and the word goes on the second line.
- A channel with a fault shows "fault" beside it, and the reason appears in words in the card's
  fault lines ("Temp 2: open circuit").

## 5. Wording

- Plain sentences in sentence case. Capitals are kept for header pills only.
- A refusal always says why and what to do: "Log in as admin to change this", "Not allowed in
  REMOTE mode", "Change this one on the web page". Error codes are not shown to operators.
- Parameter names (`iob1_ip`) appear only where they help someone find a setting, and always
  next to plain words ("No IP set (iob1_ip)").
- Times use the instrument's local standard time (D22) on a 24-hour clock. Dates are day first:
  "25/10 14:03:07" in lists, "25/10/2026" where the year matters. CSV exports keep their own
  sortable format.
- Numbers have no thousands separators. Use "." as the decimal point and show negative values
  with "-".

## 6. Touch and confirmation

- Touch targets are at least 56 px in both directions (8.4 mm on the RVT70, 6.64 px/mm),
  with 6 px between neighbours. An action happens on release over the same widget, so sliding
  off cancels it.
- These ask "Are you sure?" first: a mode change, a network change, restore defaults, a
  firmware restart, entering or leaving MAINTENANCE, and any entry flagged `PF_CONFIRM`.
- A disabled control stays visible in C_GREY, and the row says why.
- The screen dims after 10 minutes, and the first touch only wakes it. A login lapses after
  10 minutes without a touch.

## 7. Touchscreen and web page

The two interfaces show the same states, words and quality marks for the same data. The web
page may show more, such as byte buffers, arrays and commissioning, but never a different
meaning. A wording change goes into both, in the same pull request where possible.
