# Weather Sentinel — alert bench test

A repeatable check of the screens, tones and LEDs using the server's built-in
simulations. Valid from firmware **v1.2** (2026-10-04).

## Before you start

- Run it in the daytime unless you're doing the night checks below.
- While a simulation runs, the device shows a **magenta border** and a **SIMULATION**
  tag.
- If a real NWS alert becomes active, the server ends the simulation at once.
- The device checks the server **once a minute**, so allow up to 60 seconds after each
  command before judging what you see.
- Acknowledging on the device: go to the NWS Alerts page and tap the
  **TAP TO ACKNOWLEDGE** bar. Steps that wait for an acknowledgement advance on their
  own once you tap it.

**Two ways to drive the simulations.** On the device: long-press for Settings → **TEST** →
**ONE ALERT** (`nws_lifecycle`), **SEVERAL** (`multi_alert`) or **LIGHTNING**
(`lightning`), then **NEXT STEP** and **END TEST**. The page shows the current step and
updates at once after each button. Or from PowerShell, as below. Both use the same
server endpoints, so the tables below apply either way. ("`advance`" in the tables =
NEXT STEP.)

Open PowerShell and set the address once, replacing `<nas-ip>` with the NAS address:

```powershell
$ws = "http://<nas-ip>:8085/api/simulation"
```

| Action | Command |
|---|---|
| Start a scenario | `Invoke-RestMethod -Method Post -Uri "$ws/start" -ContentType "application/json" -Body '{"scenario":"multi_alert"}'` |
| Next step | `Invoke-RestMethod -Method Post -Uri "$ws/advance"` |
| Where am I? | `Invoke-RestMethod -Uri "$ws/status"` |
| Stop (back to real data) | `Invoke-RestMethod -Method Post -Uri "$ws/stop"` |

To run a different scenario, replace `multi_alert` in the start command with
`nws_lifecycle` or `lightning`. Starting a new scenario replaces the one that's running.

## What the device should do

**Tones**

- **Critical:** three-tone groups, repeating for up to 2 minutes or until acknowledged.
  Sounds even when muted or during quiet hours.
- **Warning:** two-tone groups, 5 repeats.
- **Watch:** a single chime. Silent during quiet hours (the night schedule) and when
  muted.
- **Advisory / Statement:** silent.
- **Close lightning (under 10 miles):** one short rising chirp when it first comes inside
  10 miles, once per storm. Silent during quiet hours and when muted.

**LEDs** (the first matching line wins; off means all clear)

| Condition | LEDs | Day level | Night level |
|---|---|---|---|
| Unacknowledged Critical | Red, bursts of 3 flashes | 255 | 64 |
| Unacknowledged Warning | Red, bursts of 2 flashes | 255 | 16 |
| Unacknowledged Watch | Amber, 1 flash per burst | 64 | 16 |
| Power lost, on battery | Slow blue blink | 64 | 16 |
| Alert in effect, nothing needed (acknowledged, or an Advisory or Statement) | Steady glow in the alert's colour | 32 | 16 |
| Lightning under 10 mi, or within 30 min of the last strike under 10 mi | White double flicker every 10 s | 64 | 16 |
| Lightning 10–20 mi | White single flicker every 10 s | 64 | 16 |
| Status unknown (server, NWS or Wi-Fi down) | Steady blue | 32 | 16 |
| Low battery | 5 fast blue flashes, then power-off | 64 | 16 |
| All clear | Off | — | — |

## 1. `multi_alert` — ordering, colours, every tone tier

```powershell
Invoke-RestMethod -Method Post -Uri "$ws/start" -ContentType "application/json" -Body '{"scenario":"multi_alert"}'
```

| Do | Screen | Sound | LEDs |
|---|---|---|---|
| Start | Severe Thunderstorm Warning on top (red), +1 more | Two-tone groups | Red double flashes |
| Acknowledge | Warning stays on top, +1 more | Stops | Steady red glow |
| `advance` | Flood Watch jumps to the top (amber), +2 more | One chime | Single amber flash |
| Acknowledge | Warning back on top, +2 more | — | Steady red glow |
| `advance` | Warning expired; Watch on top (amber), +1 more | — | Steady amber glow |
| `advance` | No active alerts | — | Off |

Also worth checking during this scenario: tap **+N more** to open Alert History, where
the simulated alerts appear.

## 2. `nws_lifecycle` — update, escalation to Critical, expiry

```powershell
Invoke-RestMethod -Method Post -Uri "$ws/start" -ContentType "application/json" -Body '{"scenario":"nws_lifecycle"}'
```

| Do | Screen | Sound | LEDs |
|---|---|---|---|
| Start | Special Weather Statement, awaiting acknowledgement | Silent | Steady amber glow |
| Acknowledge | Updated; acknowledgement kept | Silent | Steady amber glow |
| `advance` | Escalated to **Tornado Warning (Critical)**, long instruction text | Three-tone groups | Red triple flashes |
| Acknowledge | Acknowledged | Stops | Steady red glow |
| `advance` | Expired; no active alerts | — | Off |

## 3. `lightning` — Tempest local lightning

```powershell
Invoke-RestMethod -Method Post -Uri "$ws/start" -ContentType "application/json" -Body '{"scenario":"lightning"}'
```

| Do | Screen | Sound | LEDs |
|---|---|---|---|
| Start | Clear | — | Off |
| `advance` | Distant, 15 mi: "Lightning nearby" | None | **Single** white flicker every 10 s |
| `advance` | Close, 5.2 mi: "Lightning 5 mi"; Lightning page "Close lightning / within 10 miles" | **One rising chirp** | **Double** white flicker every 10 s |
| `advance` | Frequent and close, 3.5 mi: "Frequent lightning 4 mi" | None (same storm) | Double flicker |
| `advance` | Strikes stopped 14 min ago: "Lightning 4 mi, 14m ago"; Lightning page "14 min ago, wait 30"; other pages' strip "… -- stay in" | None | Double flicker (the 30-minute hold) |
| `advance` | Hold over: clear | — | Off |

The chirp is skipped during quiet hours and when muted; the LEDs still show it. To hear it
again, run the scenario from the start (the clear step re-arms it).

When finished: `Invoke-RestMethod -Method Post -Uri "$ws/stop"`

## 4. Mute check (optional)

1. Settings menu → **MUTE** (bottom left). It turns red and shows when the mute ends.
2. Run `multi_alert` again. The Warning should stay silent while its LEDs still flash.
3. The Now screen shows **MUTED**.
4. Run `nws_lifecycle` to the Tornado Warning step. **Critical still sounds** even
   though muted.
5. Settings menu → tap the red **MUTED** bar again to cancel it.

## 5. Status unknown

1. Stop the server container in Portainer.
2. Within about 2 minutes:
   - the LEDs turn **steady blue**;
   - the Now footer reads **NWS STATUS UNKNOWN**;
   - the Status page shows **Backend unreachable**.
3. Start the container again. Everything should recover within about a minute, and the
   LEDs should go **off**.

## 6. Power loss (USB unplugged)

1. Unplug USB. Within about 2 seconds:
   - you hear a **two-note falling chime**, even when muted;
   - the screen switches to **POWER LOST**, with "Alerts still arriving" because the
     network is still up;
   - the LEDs give a **slow blue blink**.
2. Tap: the normal pages appear. After 45 seconds without a touch, the power screen
   returns.
3. Plug USB back in: the device returns to normal within a couple of seconds.

## 7. Night check

1. Settings → **SCHEDULE**: bring the night start forward to the current time, or test
   after 10 PM.
2. Rerun the first two steps of `multi_alert`, then `nws_lifecycle` up to the Tornado
   Warning step.
3. Check that:
   - every LED state is dim (the LEDs page's night level, default 16);
   - an unacknowledged Critical is noticeably brighter (4× the night level, default 64);
   - the Watch chime is silent;
   - nothing lights up the room.
4. Set the schedule back to 10 PM–7 AM afterwards. Settings are saved (from v1.2), so a
   restart keeps whatever you leave it at.

## 8. Settings (v1.2)

1. Long-press: the **Settings menu** opens, with six tiles showing their current values,
   and **MUTE** / **DONE** along the bottom.
2. Each tile opens its page; **< BACK** returns to the menu and **DONE** to the Now screen.
3. **LEDS:** tap − or + on either level. The LEDs show amber at that level for about 3
   seconds and the page says "Showing amber at N"; then the LEDs return to normal.
4. **Saved across a restart:** change the volume, a screen brightness, an LED level and
   the night schedule; go back to the menu; restart the device (or unplug and replug it).
   Every value should be as you left it, and **mute should be off**.
5. Leave Settings alone on a page for 45 seconds: it returns to the Now screen and still
   keeps the change.

## 9. Battery run-down (occasional)

Charge the battery fully first: leave the device plugged in for several hours.

1. Unplug USB and note the time.
2. When the device shows **BATTERY LOW — Shutting down**, flashes blue 5 times and
   switches off, note the time.
3. Plug USB back in. **The device must start by itself** within a few seconds, without a
   button press. This is the critical requirement.

The shutdown threshold is 3.5 V (held for 10 s). The first run-down, on 2026-10-03, gave
about 2 hours 40 minutes on battery from a full charge (4.11 V), with BATTERY LOW at
3.48 V and an automatic restart within seconds of power returning.

## If something isn't right

Note:
- the step;
- what the screen, sound and LEDs did;
- what the table says they should do.

The serial log helps but isn't required. Watch it with
`& $pio device monitor -d "D:\Documents\GitHub\n4mi-weather-sentinel\firmware" --port COM15`
and look for the `[alarm]`, `[power]`, `[led]` and `[fetch]` lines.
