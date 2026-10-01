# LED and power diagnostic (Session 15)

A small bench sketch for the CoreS3 SE with the M5GO Bottom3 attached. It is
**not** the Weather Sentinel firmware: no Wi-Fi, no server. It answers the
hardware questions the LED and battery feature depends on. When you're done,
flash the real firmware back (bottom of this page).

## Build and flash

```powershell
$pio = (Get-ChildItem "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe").FullName
& $pio run -d "D:\Documents\GitHub\n4mi-weather-sentinel\diagnostics\led_power" -t upload --upload-port COM15
```

Optional live log (USB connected): add `& $pio device monitor -p COM15 -b 115200`
in a second window. Press the reset button once after flashing, as usual.

## Screen

- Top lines: wall (USB) power yes/no with its voltage, battery voltage and %,
  charge state, time on battery, and the last battery run saved in flash.
- Middle: what the LEDs are doing and the LED level (1–255).
- Buttons: **WALK**, **COLOR**, **BRIGHT**, **OFF**.

## Tests, in order

**1. LED count and positions (WALK).** Tap WALK. One LED lights white and the
screen shows its number. Tap WALK again to step on. The sketch drives 20 so we
find out the real count. Note:
- Which numbers light (expected 1–10), and whether 11–20 stay dark.
- Where each lit number is (e.g. "1–5 left side front to back, 6–10 right side").

**2. Colours (COLOR).** Tap COLOR, then tap it again to step through red, amber,
yellow, green, cyan, blue, purple, white. Check red is really red. If red shows
as green, the colour order is wrong; tell me and I'll fix it.

**3. Night brightness (BRIGHT + COLOR), in the dark bedroom.** BRIGHT steps the
level 1, 2, 4 … 255. At bedtime with the room dark, note:
- the lowest level where red and amber are clearly visible but don't light up the room;
- whether amber still looks different from red at that level (low levels lose
  colour detail);
- which of cyan, blue, purple and white is most distinct from red and amber (a
  candidate for lightning and for low battery).

**4. Day brightness.** The same in daylight: the lowest level that catches your eye
across the room.

**5. Does the power chip see the battery and the power loss?** With USB in, note
the battery voltage, % and charge state. Unplug USB:
- Does the device stay on?
- Does "Wall power" change to **NO** within a second or two?
- Does "On battery" start counting?
- Do the LEDs still light? Tap COLOR to check.

Plug USB back in: does Wall power go back to **YES**? If Wall power never changes,
or the battery reads 0 mV, the chip can't see the M5GO battery, and we'll need
a different approach.

**6. Restart after power-off (the critical one).** Tap OFF, then OFF again within
3 seconds. With USB unplugged, the device switches off. Now plug USB in and
wait 10 seconds without touching anything. **Does it start up by itself?** Then
repeat with USB already plugged in when you tap OFF: does it stay off, or come
straight back?

**7. Battery run-down (optional, can run overnight).** Charge the battery fully
first: leave it plugged in until it stops charging. Then:
1. Set COLOR on amber at your chosen night level.
2. Unplug USB and leave it until the device dies.
3. Plug back in.
4. If it doesn't start by itself, press the power button.

The "Last" line then shows how many minutes it ran and the final voltage. It's
saved once a minute, so it's accurate to within a minute. That sets the
low-battery shutdown point.

## Report back

For each test, give the numbers or yes/no answers. Photos of the LEDs at night
help, but they're not required.

## Put the real firmware back

```powershell
& $pio run -d "D:\Documents\GitHub\n4mi-weather-sentinel\firmware" -t upload --upload-port COM15
```

The diagnostic keeps its battery log in its own flash area (`diag`). That area
doesn't touch anything the real firmware uses.
