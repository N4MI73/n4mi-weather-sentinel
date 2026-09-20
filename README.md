# Weather Sentinel

A bedside weather and alert appliance built on the M5Stack CoreS3 SE. It answers three
questions without becoming a full weather platform:

1. **What's happening in the yard right now?** — real-time conditions from a WeatherFlow
   Tempest station (temperature, humidity, wind, gust, rain, station pressure).
2. **Is a locally measured condition becoming noteworthy?** — Tempest-derived threshold
   events, starting with nearby lightning (filtered to within 10 miles, classified as
   sporadic or frequent rather than shown as raw numbers).
3. **Is an official NWS alert currently in effect?** — polled from the National Weather
   Service's public API for the local zone, with full lifecycle tracking (new, updated,
   escalated, expired) and acknowledgement.

The device stays dark and quiet during ordinary bedroom use and becomes conspicuous only
when conditions actually warrant it. It's explicitly **supplemental** — Wireless Emergency
Alerts, weather apps, NOAA Weather Radio, and other existing warning methods remain
independent and are never represented as replaced.

## How it's built

Two independent pieces, connected over the local network:

- **`firmware/`** — runs on the CoreS3 SE itself (ESP32-S3, PlatformIO + Arduino,
  M5Unified/M5GFX for display and touch). Connects to Wi-Fi, polls the backend every 60
  seconds, and renders five screens: a glanceable "Now" view plus four detail pages (Wind
  & Rain, Lightning, NWS Alerts, Status). Time is synced via NTP.
- **`server/`** — a small always-on Python/Flask service, deployed to a NAS via Docker/
  Portainer. Listens directly for the Tempest hub's local UDP broadcast (no cloud API
  call, no token needed) and separately polls the NWS API, exposing both as a single
  compact JSON endpoint (`GET /api/conditions`) the device consumes.

Running its own backend (rather than depending on other household dashboards) means the
device stays useful even if other, less consistently-running services are offline
overnight.

## Current status

Both the firmware and server are real, deployed, and talking to each other:

- All five screens display live data on real hardware.
- The server is deployed on the NAS, has survived a real container restart without
  losing alert state, and correctly distinguishes "no active alerts" from "couldn't
  check" — the latter is never allowed to look like the former.
- Lightning's distance filter and rate classification, and the NWS alert lifecycle
  state machine, are both built and tested, though real-world validation against an
  actual storm or active alert hasn't happened yet — that can't be forced.
- Firmware-side alert acknowledgement isn't wired up yet; the device currently reads and
  displays alert content but doesn't yet mark alerts as acknowledged.

For exact per-session status, hardware validation detail, the full API contract, and a
complete decision history, see `Weather_Sentinel_Project_Brief.md` at the repo root —
that file, not this one, is the authoritative operational source of truth for the
project.

## Repo layout

```
n4mi-weather-sentinel/
├── .gitignore
├── README.md
├── Weather_Sentinel_Project_Brief.md
├── firmware/
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp
│       ├── wifi_credentials.h            (gitignored — real credentials)
│       └── wifi_credentials.h.example    (placeholder template)
└── server/
    ├── weather_sentinel_server.py
    ├── Dockerfile
    └── docker-compose.yml
```

## Building the firmware

```powershell
platformio run -d "D:\Documents\GitHub\n4mi-weather-sentinel\firmware" -t upload --upload-port COM15
```

Copy `firmware/src/wifi_credentials.h.example` to `wifi_credentials.h` and fill in real
Wi-Fi credentials before building — this stopgap will eventually be replaced by an
on-device captive portal for real bedside deployment.

## Running the server

Deployed via Portainer's Repository build method (see the project brief for the exact
walkthrough). Requires one environment variable, `NWS_CONTACT_INFO`, in the format NWS
requests: `(appname, contact@email.com)`. No API keys or tokens are needed for either
Tempest or NWS.
