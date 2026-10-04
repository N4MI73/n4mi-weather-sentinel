# Weather Sentinel — server

A small, always-on Python/Flask service that the bedside device polls:

- It listens for the Tempest hub's **local UDP broadcast** on port 50222: observations
  (`obs_st`) and individual lightning strikes (`evt_strike`). There's no WeatherFlow
  cloud call and no token.
- It polls the **NWS active-alerts API** for one zone (`NWS_ZONE_ID`, default `GAC073`)
  every 5 minutes.
- It classifies alerts (Critical / Warning / Watch / Advisory / Informational), tracks
  each one by its NWS id, records acknowledgements, and keeps expired alerts for 24 hours
  for the device's Alert History page.
- It serves everything as compact JSON on **port 8085**.

## Requirements

- **Same local network as the Tempest hub.** The hub broadcasts its data on the local
  network, and that broadcast doesn't cross routers or separate VLANs, so the machine
  running this server must be on the same network segment as the hub.
- **Incoming UDP on port 50222** allowed on that machine.
- **Host networking in Docker** (see below). A container on Docker's default network never
  hears the broadcast.
- **Your own NWS zone.** Set `NWS_ZONE_ID` in Portainer (see Environment variables) to
  your county code (e.g. `GAC073`) or forecast-zone code (e.g. `GAZ073`). Both are listed
  on weather.gov.
- **No WeatherFlow account, token or cloud access** is needed. The server works entirely
  from the hub's local broadcast.

## Deploying (NAS, Portainer)

It's deployed with Portainer's **Repository** build method: Portainer builds the image
from this folder's `Dockerfile` instead of pulling a pre-built one.

1. Create the data folder on the NAS if it doesn't exist:
   `/volume2/docker/weather-sentinel/data`.
2. In Portainer, create a stack from this Git repository with the compose path
   `server/docker-compose.yml`.
3. Set the environment variables (below) and deploy.
4. To update after a push, use **Pull and redeploy**. This only works because the
   compose file sets `pull_policy: build`; without it, Portainer tries to pull an image
   that doesn't exist and fails.
5. Check it's running: `http://<nas-ip>:8085/healthz` should return `{"status": "ok"}`.
   The container log should show `[config] lightning filter radius = N mi`.

### Compose settings that matter

- **`network_mode: host` is required.** A container on Docker's default network never
  receives the hub's UDP broadcast, and nothing reports the failure. With host
  networking there's no `ports:` section; the app is reachable at the NAS's own address
  on port 8085.
- **Volume `/volume2/docker/weather-sentinel/data:/data`** holds `alerts_state.json`,
  the alert ids, acknowledgements and 24-hour history. Without it, every redeploy would
  make still-active alerts sound again as if they were new.

## Environment variables

| Variable | Required | Meaning |
|---|---|---|
| `NWS_CONTACT_INFO` | yes | The identification NWS asks for: `(appname, contact@email.com)`. Set it only in Portainer; never commit a real address. |
| `NWS_ZONE_ID` | no (default `GAC073`) | The NWS zone whose alerts are watched: a county code (`GAC073`) or forecast-zone code (`GAZ073`). A malformed value falls back to the default, and the log says so. The log shows the zone in use at startup: `[config] NWS zone = …`. |
| `LIGHTNING_FILTER_RADIUS_MI` | no (default 10) | Lightning distance cutoff in miles. Blank or invalid values fall back to 10, and the log says so. |
| `ALERTS_PERSISTENCE_PATH` | no (default `/data/alerts_state.json`) | Where alert state is saved. The tests use it to write to a temporary folder. |

A Portainer variable reaches the container **only if the compose file passes it
through**, e.g. `LIGHTNING_FILTER_RADIUS_MI=${LIGHTNING_FILTER_RADIUS_MI:-10}` under
`environment:`. A missing closing brace gives Portainer's "invalid interpolation format"
error.

## Only if another program on the same machine hears the broadcast (e.g. WeeWX)

Skip this section if nothing else on the server's machine listens for the Tempest
broadcast. It applies here because WeeWX runs on the same NAS, also with host networking.
Two programs on one machine can share the broadcast's UDP port only if **both** agree to
share it:

- this server's socket sets `SO_REUSEPORT` (already in the code);
- WeeWX's `weatherflowudp` driver needs `share_socket = True` in `weewx.conf`.

If only one side opts in, the second program to start fails with
`OSError: [Errno 98] Address already in use`. The same applies to any future service
that listens for this broadcast.

## Endpoints

| Method | Path | Used for |
|---|---|---|
| GET | `/api/conditions` | Everything the device shows: `tempest`, `lightning` (including a `close` block), `nws` (with `alerts[]`), and a `simulation` flag. |
| POST | `/api/alerts/ack` | Body `{"id": "<alert id>"}`. Marks an alert acknowledged. |
| GET | `/api/alerts/history` | The last 24 hours of alerts (at most 12), fetched when the device's Alert History page opens. |
| POST | `/api/simulation/start` | Body `{"scenario": "nws_lifecycle" \| "multi_alert" \| "lightning"}`. |
| POST | `/api/simulation/advance` | Move to the next step of the running scenario. |
| POST | `/api/simulation/stop` | Go back to serving real data. |
| GET | `/api/simulation/status` | The current scenario and step. |
| GET | `/healthz` | Liveness check. |

**Alert order in `nws.alerts[]`:**
1. An alert that is *sounding* and still unacknowledged (Watch or higher) comes first.
2. Everything else is ordered by level: Critical, Warning, Watch, Advisory,
   Informational.
3. Within a level, the newest comes first.

The device acts on the first entry, so this order is the device's priority rule.

**Close lightning.** A strike under 10 miles is "close". `lightning.close` reports
whether a close strike happened in the last 30 minutes, with its distance, time and
minutes since. The 30 minutes follow NWS guidance to wait 30 minutes after the last
thunder, so the close state outlasts the 10-minute strike window. Both values are set in
the code (`LIGHTNING_CLOSE_RADIUS_MI`, `LIGHTNING_CLOSE_HOLD_MINUTES`). A server restart
forgets the last close strike, just as it forgets the strike window.

**Staleness is reported, not hidden:**
- `tempest.available` goes false after 5 minutes without an observation.
- `nws.available` goes false when a poll fails, and the last good alerts are kept.

The device turns these into "unknown", never into "clear".

**LAN only.** The acknowledgement endpoint has no authentication. Port 8085 must never be
forwarded or otherwise exposed to the internet.

## Simulations (bench testing)

A simulation replaces what `/api/conditions` serves with scripted data in exactly the
same shape. The device shows a magenta border and a **SIMULATION** tag while one is
running. Acknowledging on the device genuinely advances the steps that wait for it. If a
real NWS alert becomes active, the server ends the simulation immediately, so a test can
never hide a real warning.

```powershell
$ws = "http://<nas-ip>:8085/api/simulation"
Invoke-RestMethod -Method Post -Uri "$ws/start" -ContentType "application/json" -Body '{"scenario":"multi_alert"}'
Invoke-RestMethod -Method Post -Uri "$ws/advance"
Invoke-RestMethod -Method Post -Uri "$ws/stop"
```

- **`nws_lifecycle`:** one alert, going new → acknowledged → updated → escalated to a
  Tornado Warning with long text → expired.
- **`multi_alert`:** several alerts at once, exercising the ordering, colours,
  "+N more", Alert History and every tone tier.
- **`lightning`:** clear → distant (15 mi) → close (5.2 mi) → frequent and close →
  strikes stopped 14 minutes ago (the 30-minute close hold) → hold over, clear.

## Tests

```powershell
pip install flask requests
python server\tests\test_alert_priority.py
python server\tests\test_alert_history.py
python server\tests\test_nws_zone.py
python server\tests\test_lightning_close.py
```

Each file runs on its own with plain Python. They need no network, NAS or device, and
write state to a temporary folder. The Dockerfile copies only
`weather_sentinel_server.py`, so the tests never go into the container.
