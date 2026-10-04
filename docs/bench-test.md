# Weather Sentinel — alert bench test

A repeatable check of the screens, tones and LEDs using the server's built-in
simulations. Valid from firmware **v1.1** (2026-10-03).

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

**LEDs** (the first matching line wins; off means all clear)

| Condition | LEDs | Day level | Night level |
|---|---|---|---|
| Unacknowledged Critical | Red, bursts of 3 flashes | 255 | 64 |
| Unacknowledged Warning | Red, bursts of 2 flashes | 255 | 16 |
| Unacknowledged Watch | Amber, 1 flash per burst | 64 | 16 |
| Power lost, on battery | Slow blue blink | 64 | 16 |
| Alert in effect, nothing needed (acknowledged, or an Advisory or Statement) | Steady glow in the alert's colour | 32 | 16 |
| Lightning within the radius | White double flicker every 10 s | 64 | 16 |
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

| Do | Screen | LEDs |
|---|---|---|
| Start | Clear | Off |
| `advance` | Lightning nearby (sporadic) | White double flicker every 10 s |
| `advance` | Frequent lightning | White double flicker every 10 s |
| `advance` | Aged out; clear | Off |

When finished: `Invoke-RestMethod -Method Post -Uri "$ws/stop"`

## 4. Mute check (optional)

1. Settings → Mute page → **Mute**.
2. Run `multi_alert` again. The Warning should stay silent while its LEDs still flash.
3. The Now screen shows **MUTED**.
4. Run `nws_lifecycle` to the Tornado Warning step. **Critical still sounds** even
   though muted.
5. Tap Mute again to cancel it.

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

1. Settings → Schedule page: bring the night start forward to the current time, or test
   after 10 PM.
2. Rerun the first two steps of `multi_alert`, then `nws_lifecycle` up to the Tornado
   Warning step.
3. Check that:
   - every LED state is dim (level 16);
   - an unacknowledged Critical is noticeably brighter (level 64);
   - the Watch chime is silent;
   - nothing lights up the room.
4. Set the schedule back to 10 PM–7 AM afterwards. Settings aren't saved across a
   restart yet, so a restart also puts it back.

## 8. Battery run-down (occasional)

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
