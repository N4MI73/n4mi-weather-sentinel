#!/usr/bin/env python3
"""
Weather Sentinel -- backend server, early build.

Combines the confirmed-working Tempest UDP listener (see
tempest_udp_test.py) and the confirmed-working NWS poller (see
nws_alert_test.py) into one long-running service with an HTTP endpoint,
so the device will eventually be able to fetch current conditions and
alert status over the network.

STATUS: general conditions + raw active-alert polling + lightning
10-mile filter/classification. NOT yet included (each a separate,
deliberately deferred next step):
  - Daily rain accumulation, pressure trend, feels-like temperature,
    sea-level pressure adjustment (all deferred earlier)
  - Alert LIFECYCLE tracking (new/updated/escalated/canceled/expired,
    acknowledgement state) -- this file reports NWS's raw active-alert
    list every poll, with no memory of what changed since last time
  - Persistence across restarts
  - Distance/geographic filtering beyond the single fixed zone (GAC073)

Lightning classification (LIGHTNING_WINDOW_MINUTES,
LIGHTNING_FREQUENT_THRESHOLD near the top of this file) uses PLACEHOLDER
values, not calibrated ones -- these need Dan's own bench-testing once
real storms exist to test against.

Environment variables:
    NWS_CONTACT_INFO  Required before NWS polling will run. Format:
                       "(yourapp.com, contact@email.com)" -- NWS requires
                       this to identify traffic. Deliberately NOT
                       hardcoded here (unlike the throwaway
                       nws_alert_test.py script) since this file is
                       meant to end up in the public repo, and a real
                       email address shouldn't be committed to it.

Run directly:
    pip3 install flask requests --break-system-packages
    export NWS_CONTACT_INFO="(n4mi-weather-sentinel, your@email.com)"
    python3 weather_sentinel_server.py

Then from any machine on the LAN:
    curl http://<nas-ip>:8085/api/conditions
"""

import os
import socket
import json
import time
import threading
from datetime import datetime, timezone, timedelta
from flask import Flask, jsonify, request
import requests

UDP_PORT = 50222
HTTP_PORT = 8085  # matches the port reserved for this project

NWS_ZONE_ID = "GAC073"  # Columbia County, GA -- confirmed independently
                         # in Personal Portal and Storm Alert System
NWS_CONTACT_INFO = os.environ.get(
    "NWS_CONTACT_INFO", "(n4mi-weather-sentinel, CHANGE-ME@example.com)"
)
NWS_URL = f"https://api.weather.gov/alerts/active/zone/{NWS_ZONE_ID}"
NWS_HEADERS = {
    "User-Agent": NWS_CONTACT_INFO,
    "Accept": "application/geo+json",
}
NWS_POLL_INTERVAL_SEC = 300  # 5 minutes -- matches Personal Portal's
                              # already-established NWS cache interval

# Persistence: NWS alert lifecycle state only (IDs, ack state, and
# enough content to correctly classify the next poll as "unchanged"
# rather than "new"). Deliberately NOT covering lightning's rolling
# strike window -- losing that on a restart is minor and self-healing
# within one window's length, unlike every NWS alert wrongly re-sounding
# as brand new, which is the actual guardrail this exists for.
ALERTS_PERSISTENCE_PATH = os.environ.get(
    "ALERTS_PERSISTENCE_PATH", "/data/alerts_state.json"
)

MS_TO_MPH = 2.23694


def c_to_f(c):
    return c * 9 / 5 + 32


MB_TO_INHG = 0.0295300
MM_TO_IN = 0.0393701
KM_TO_MI = 0.621371

COMPASS_POINTS = [
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
]


def degrees_to_compass(deg):
    index = round(deg / 22.5) % 16
    return COMPASS_POINTS[index]


def iso_time(epoch):
    return datetime.fromtimestamp(epoch, tz=timezone.utc).isoformat()


# --- Derived values, using WeatherFlow's own published definitions so the
# device agrees with the Tempest app (weatherflow.github.io/SmartWeather/
# api/derived-metric-formulas.html). ---

def rain_rate_level(rate_mm_per_hr):
    """Tempest's rain-intensity words. The rate is the latest one-minute
    accumulation extrapolated to an hour, so it can change minute to
    minute as showers come and go -- same as the app."""
    if rate_mm_per_hr <= 0:
        return "none"
    if rate_mm_per_hr < 0.25:
        return "very_light"
    if rate_mm_per_hr < 1.0:
        return "light"
    if rate_mm_per_hr < 4.0:
        return "moderate"
    if rate_mm_per_hr < 16.0:
        return "heavy"
    if rate_mm_per_hr < 50.0:
        return "very_heavy"
    return "extreme"


def heat_index_f(temp_f, rh_pct):
    return (-42.379 + 2.04901523 * temp_f + 10.1433127 * rh_pct
            - 0.22475541 * temp_f * rh_pct - 6.83783e-3 * temp_f ** 2
            - 5.481717e-2 * rh_pct ** 2 + 1.22874e-3 * temp_f ** 2 * rh_pct
            + 8.5282e-4 * temp_f * rh_pct ** 2
            - 1.99e-6 * temp_f ** 2 * rh_pct ** 2)


def wind_chill_f(temp_f, wind_mph):
    v16 = wind_mph ** 0.16
    return 35.74 + 0.6215 * temp_f - 35.75 * v16 + 0.4275 * temp_f * v16


def feels_like_f(temp_f, rh_pct, wind_mph):
    """WeatherFlow's rule: heat index at or above 80F and 40% RH; wind
    chill at or below 50F with wind over 3 mph; otherwise the air
    temperature itself."""
    if temp_f >= 80 and rh_pct >= 40:
        return heat_index_f(temp_f, rh_pct)
    if temp_f <= 50 and wind_mph > 3:
        return wind_chill_f(temp_f, wind_mph)
    return temp_f


# Shared state, protected by a lock since the UDP listener thread writes
# to it and Flask's request-handling thread(s) read from it.
state_lock = threading.Lock()
latest_conditions = {
    "available": False,
    "observed_at": None,
    "temperature_f": None,
    "humidity_percent": None,
    "wind_mph": None,
    "gust_mph": None,
    "wind_direction": None,
    "pressure_inhg_station": None,  # NOT sea-level-adjusted -- deferred
    "rain_this_interval_in": None,   # kept for compatibility; the device
                                     # now uses rain_rate_* below instead
    "rain_rate_in_hr": None,
    "rain_rate_level": None,
    "feels_like_f": None,
}

# When the last obs_st packet was RECEIVED, on this server's own monotonic
# clock (immune to clock skew between the hub and this host, and to
# wall-clock changes). "available" above only ever meant "at least one
# packet has ever arrived"; api_conditions() combines it with this to
# report whether observations are actually still flowing.
last_obs_received_monotonic = None
TEMPEST_STALE_SEC = 300  # obs_st normally arrives about once a minute;
                          # PLACEHOLDER -- 5 minutes tolerates a few drops.

# Message types seen since startup, so the log records the first packet
# of each kind (e.g. whether evt_strike has EVER arrived on this port).
seen_message_types = set()

# --- Lightning: 10-mile filter + heavy/sporadic classification ---
# Confirmed requirement: only strikes within this radius matter to this
# device at all -- farther strikes are outside scope, not tracked.
def _radius_from_env(default=10.0):
    """LIGHTNING_FILTER_RADIUS_MI from the environment (Portainer), so the
    radius can be widened for testing and put back without a code change.
    A missing, non-numeric, or non-positive value falls back to the
    confirmed 10-mile requirement, loudly, rather than silently."""
    raw = os.environ.get("LIGHTNING_FILTER_RADIUS_MI")
    if raw is None or raw.strip() == "":
        return default
    try:
        value = float(raw)
        if value <= 0:
            raise ValueError("must be positive")
        return value
    except ValueError as e:
        print(f"[config] IGNORING bad LIGHTNING_FILTER_RADIUS_MI={raw!r} ({e}); "
              f"using {default:g} mi", flush=True)
        return default


LIGHTNING_FILTER_RADIUS_MI = _radius_from_env()

# PLACEHOLDERS -- these need Dan's own bench-testing calibration once
# real hardware/real storms exist to test against, per the project's
# own guardrail against guessed defaults. Not confirmed-correct values.
LIGHTNING_WINDOW_MINUTES = 10
LIGHTNING_FREQUENT_THRESHOLD = 3  # this many or more strikes in the
                                   # window = "frequent"; fewer = "sporadic"

# Raw list of {"distance_mi": float, "at": datetime} for strikes within
# the filter radius, pruned to the rolling window on every read AND on
# every new strike -- not just when a new strike arrives, since a storm
# that passed 20 minutes ago must stop showing "frequent" even with no
# new packets to trigger a re-check.
recent_strikes = []

latest_nws = {
    # "available" is true ONLY after a successful poll -- per the
    # project's own guardrail, an empty alert list must never be shown
    # unless it came from a real, fresh, successful retrieval. A failed
    # poll leaves available=False without touching tracked_alerts below,
    # so stale data stays visible (with available correctly flagging it
    # as untrustworthy) rather than being cleared outright.
    "available": False,
    "last_polled_at": None,
}


def handle_obs_st(msg):
    global last_obs_received_monotonic
    obs = msg["obs"][0]

    # Diagnostic: obs_st also carries the sensor's own per-interval
    # lightning summary (index 14 = average distance in km, 15 = strike
    # count). Log it whenever nonzero -- if it shows strikes while no
    # evt_strike packets are arriving, per-strike events are not being
    # delivered to this listener. Log-only; does not affect any state.
    if len(obs) > 15 and obs[15]:
        print(f"[udp] obs_st reports {obs[15]} strike(s) this interval, "
              f"avg distance {obs[14]} km ({float(obs[14]) * KM_TO_MI:.1f} mi)", flush=True)

    # Compute everything first, from the RAW values, before touching shared
    # state -- so a bad packet can't leave half-updated conditions behind,
    # and so rain rate isn't computed from an already-rounded number
    # (one minute of "light" rain is ~0.00016 in and would round to zero).
    temp_f_raw = c_to_f(obs[7])
    wind_mph_raw = obs[2] * MS_TO_MPH
    rain_rate_mm_hr = obs[12] * 60.0   # last-minute accumulation -> per hour
    computed = {
        "observed_at": iso_time(obs[0]),
        "wind_mph": round(wind_mph_raw, 1),
        "gust_mph": round(obs[3] * MS_TO_MPH, 1),
        "wind_direction": degrees_to_compass(obs[4]),
        "pressure_inhg_station": round(obs[6] * MB_TO_INHG, 2),
        "temperature_f": round(temp_f_raw, 1),
        "humidity_percent": round(obs[8]),
        "rain_this_interval_in": round(obs[12] * MM_TO_IN, 3),
        "rain_rate_in_hr": round(rain_rate_mm_hr * MM_TO_IN, 2),
        "rain_rate_level": rain_rate_level(rain_rate_mm_hr),
        "feels_like_f": round(feels_like_f(temp_f_raw, obs[8], wind_mph_raw), 1),
    }

    with state_lock:
        last_obs_received_monotonic = time.monotonic()
        latest_conditions["available"] = True
        latest_conditions.update(computed)


def _prune_recent_strikes_locked():
    """Remove strikes older than the classification window. Caller must
    already hold state_lock."""
    cutoff = datetime.now(timezone.utc) - timedelta(minutes=LIGHTNING_WINDOW_MINUTES)
    recent_strikes[:] = [s for s in recent_strikes if s["at"] >= cutoff]


def handle_evt_strike(msg):
    epoch, distance_km, energy = msg["evt"]
    distance_mi = distance_km * KM_TO_MI
    age_sec = time.time() - epoch

    if distance_mi > LIGHTNING_FILTER_RADIUS_MI:
        # Outside the confirmed 10-mile scope -- not relevant to this
        # device, deliberately not tracked at all.
        print(f"[strike] {distance_km} km ({distance_mi:.1f} mi), stamped {age_sec:.0f}s ago "
              f"-> IGNORED (beyond {LIGHTNING_FILTER_RADIUS_MI:g} mi)", flush=True)
        return

    strike_time = datetime.fromtimestamp(epoch, tz=timezone.utc)
    with state_lock:
        recent_strikes.append({"distance_mi": distance_mi, "at": strike_time})
        _prune_recent_strikes_locked()
        kept = len(recent_strikes)
    # If the strike's own timestamp is already older than the window,
    # pruning drops it immediately -- say so, since that looks identical
    # to "never arrived" from the API side.
    outcome = "TRACKED" if kept else (
        f"DROPPED -- stamp is older than the {LIGHTNING_WINDOW_MINUTES}-minute window")
    print(f"[strike] {distance_km} km ({distance_mi:.1f} mi), stamped {age_sec:.0f}s ago "
          f"-> {outcome} (in-window strikes now: {kept})", flush=True)


def get_lightning_status():
    """Prune and classify against the current moment -- called on every
    API request, not just when a new strike arrives, so a storm that
    has moved on correctly stops showing as active."""
    with state_lock:
        _prune_recent_strikes_locked()
        count = len(recent_strikes)

        if count == 0:
            return {
                "active": False,
                "level": "none",
                "closest_distance_mi": None,
                "most_recent_strike_at": None,
                "most_recent_strike_epoch": None,
                "strike_count_recent": 0,
                "window_minutes": LIGHTNING_WINDOW_MINUTES,
                "filter_radius_mi": LIGHTNING_FILTER_RADIUS_MI,
            }

        closest = min(s["distance_mi"] for s in recent_strikes)
        most_recent = max(s["at"] for s in recent_strikes)
        level = "frequent" if count >= LIGHTNING_FREQUENT_THRESHOLD else "sporadic"

        return {
            "active": True,
            "level": level,
            "closest_distance_mi": round(closest, 1),
            "most_recent_strike_at": most_recent.isoformat(),
            # Epoch seconds, so the device can convert to LOCAL time the
            # same way it does for its clock. The ISO string above is UTC,
            # which the device previously displayed as if it were local.
            "most_recent_strike_epoch": int(most_recent.timestamp()),
            "strike_count_recent": count,
            "window_minutes": LIGHTNING_WINDOW_MINUTES,
            "filter_radius_mi": LIGHTNING_FILTER_RADIUS_MI,
        }



# --- NWS alert lifecycle tracking ---
# Tracked by stable NWS `id`, not by event-name/text matching, per the
# project's own confirmed requirement.

# PROPOSAL, not yet confirmed with Dan -- maps raw NWS fields to the
# device's five-tier vocabulary (Critical/Warning/Watch/Advisory/
# Informational). Primarily event-name based since that's the most
# reliable signal for the Warning/Watch/Advisory distinction; CAP's
# `severity` field splits Critical out from an ordinary Warning.
LEVEL_RANK = {
    "informational": 0,
    "advisory": 1,
    "watch": 2,
    "warning": 3,
    "critical": 4,
}


def classify_level(props):
    event = (props.get("event") or "").lower()
    severity = props.get("severity") or "Unknown"

    if "warning" in event:
        return "critical" if severity == "Extreme" else "warning"
    if "watch" in event:
        return "watch"
    if "advisory" in event:
        return "advisory"
    return "informational"


# id -> alert record. In-memory only -- persistence across a restart is
# separate, deliberately deferred work (see project brief).
tracked_alerts = {}

ALERT_EXPIRY_CLEANUP_MINUTES = 30  # how long an expired alert stays
                                     # visible internally before being
                                     # dropped, bounding memory growth
                                     # without needing real persistence


def process_nws_alerts_locked(raw_props_list):
    """Update tracked_alerts against a fresh poll's raw properties list.
    Caller must already hold state_lock."""
    now_iso = datetime.now(timezone.utc).isoformat()
    current_ids = set()

    for props in raw_props_list:
        alert_id = props.get("id")
        if not alert_id:
            continue
        current_ids.add(alert_id)
        level = classify_level(props)

        if alert_id not in tracked_alerts:
            tracked_alerts[alert_id] = {
                "id": alert_id,
                "event": props.get("event"),
                "severity": props.get("severity"),
                "urgency": props.get("urgency"),
                "certainty": props.get("certainty"),
                "onset": props.get("onset"),
                "expires": props.get("expires"),
                "headline": props.get("headline"),
                "instruction": props.get("instruction"),
                "level": level,
                "acknowledged": False,
                "last_transition": "new",
                "first_seen_at": now_iso,
                "last_updated_at": now_iso,
            }
            continue

        existing = tracked_alerts[alert_id]
        old_level = existing["level"]
        content_changed = (
            existing["event"] != props.get("event")
            or existing["headline"] != props.get("headline")
            or existing["instruction"] != props.get("instruction")
            or existing["expires"] != props.get("expires")
            or old_level != level
        )

        if not content_changed:
            existing["last_transition"] = "unchanged"
        elif LEVEL_RANK.get(level, 0) > LEVEL_RANK.get(old_level, 0):
            # Escalation -- confirmed policy: clears acknowledgement,
            # forcing the alert to sound again at the new level.
            existing["acknowledged"] = False
            existing["last_transition"] = "escalated"
        else:
            # Content changed but didn't escalate -- ack is preserved.
            existing["last_transition"] = "updated"

        existing["event"] = props.get("event")
        existing["severity"] = props.get("severity")
        existing["urgency"] = props.get("urgency")
        existing["certainty"] = props.get("certainty")
        existing["onset"] = props.get("onset")
        existing["expires"] = props.get("expires")
        existing["headline"] = props.get("headline")
        existing["instruction"] = props.get("instruction")
        existing["level"] = level
        existing["last_updated_at"] = now_iso

    # Anything tracked but no longer in NWS's active list has expired.
    # NWS's active-alerts feed doesn't distinguish a true cancellation
    # from natural expiration -- both simply disappear from it, so both
    # are treated the same way here.
    for alert_id, existing in tracked_alerts.items():
        if alert_id not in current_ids and existing["last_transition"] != "expired":
            existing["last_transition"] = "expired"
            existing["last_updated_at"] = now_iso

    # Bound memory growth: drop alerts that have been expired for a
    # while, rather than keeping every alert ever seen forever. This is
    # not real persistence -- just keeps a long-running process's memory
    # from growing unbounded across weeks of uptime.
    cutoff = datetime.now(timezone.utc) - timedelta(minutes=ALERT_EXPIRY_CLEANUP_MINUTES)
    to_remove = [
        aid for aid, a in tracked_alerts.items()
        if a["last_transition"] == "expired"
        and datetime.fromisoformat(a["last_updated_at"]) < cutoff
    ]
    for aid in to_remove:
        del tracked_alerts[aid]


def load_persisted_alerts():
    """Load tracked_alerts from disk at startup, if a persisted file
    exists. A missing file (first-ever run, or a fresh volume) or any
    read/parse problem is treated as "nothing to load" rather than a
    fatal error -- the server should still start and just begin
    tracking fresh, same as it always has."""
    try:
        with open(ALERTS_PERSISTENCE_PATH, "r") as f:
            loaded = json.load(f)
        with state_lock:
            tracked_alerts.clear()
            tracked_alerts.update(loaded)
        print(f"[persistence] Loaded {len(loaded)} tracked alert(s) from {ALERTS_PERSISTENCE_PATH}")
    except FileNotFoundError:
        print(f"[persistence] No existing state file at {ALERTS_PERSISTENCE_PATH} -- starting fresh")
    except (json.JSONDecodeError, OSError) as e:
        print(f"[persistence] Could not load state from {ALERTS_PERSISTENCE_PATH}: {e} -- starting fresh")


def save_persisted_alerts_locked():
    """Write the current tracked_alerts to disk. Caller must already
    hold state_lock. A write failure is logged, not fatal -- losing the
    ability to persist shouldn't crash an otherwise-working server."""
    try:
        with open(ALERTS_PERSISTENCE_PATH, "w") as f:
            json.dump(tracked_alerts, f)
    except OSError as e:
        print(f"[persistence] Failed to save state to {ALERTS_PERSISTENCE_PATH}: {e}")


def poll_nws_once():
    try:
        response = requests.get(NWS_URL, headers=NWS_HEADERS, timeout=10)
        response.raise_for_status()
        data = response.json()
        features = data.get("features", [])
        raw_props_list = [f.get("properties", {}) for f in features]

        with state_lock:
            latest_nws["available"] = True
            latest_nws["last_polled_at"] = datetime.now(timezone.utc).isoformat()
            process_nws_alerts_locked(raw_props_list)
            save_persisted_alerts_locked()

        print(f"[nws] Poll succeeded -- {len(raw_props_list)} active alert(s) from NWS")

    except requests.RequestException as e:
        with state_lock:
            latest_nws["available"] = False
        print(f"[nws] Poll FAILED: {e} -- marked unavailable, will retry next cycle")


def nws_poller_thread():
    if "CHANGE-ME" in NWS_CONTACT_INFO:
        print("[nws] NWS_CONTACT_INFO not configured (still has the")
        print("      placeholder email) -- NWS polling will NOT run.")
        print("      Set the NWS_CONTACT_INFO environment variable to a")
        print("      real contact address before this will work.")
        return  # Tempest side keeps running either way; only NWS is skipped

    print(f"[nws] Starting poller, every {NWS_POLL_INTERVAL_SEC}s, "
          f"zone {NWS_ZONE_ID}")
    while True:
        poll_nws_once()
        time.sleep(NWS_POLL_INTERVAL_SEC)


def udp_listener_thread():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # SO_REUSEPORT (not just SO_REUSEADDR) is what actually allows two
    # separate processes -- this one and WeeWX's own container, also
    # using network_mode: host to receive this exact same broadcast --
    # to both bind port 50222 and each get their own copy of every
    # packet. Discovered necessary in practice: SO_REUSEADDR alone
    # produced "Address already in use" against the real NAS deployment.
    if hasattr(socket, "SO_REUSEPORT"):
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(("", UDP_PORT))
    print(f"[udp] Listening on port {UDP_PORT}")

    while True:
        try:
            data, addr = sock.recvfrom(4096)
        except OSError as e:
            print(f"[udp] recvfrom error: {e!r} -- retrying", flush=True)
            time.sleep(1)
            continue

        # A malformed or unexpected packet must never kill this thread:
        # the HTTP side would keep serving frozen data with nothing
        # visibly wrong. Log it (with the raw packet) and carry on.
        try:
            msg = json.loads(data.decode("utf-8"))
            msg_type = msg.get("type")
            if msg_type not in seen_message_types:
                seen_message_types.add(msg_type)
                print(f"[udp] first packet of type {msg_type!r} since startup", flush=True)

            if msg_type == "obs_st":
                handle_obs_st(msg)
                print(f"[udp] obs_st received, temp={latest_conditions['temperature_f']}F")
            elif msg_type == "evt_strike":
                handle_evt_strike(msg)
        except Exception as e:  # noqa: BLE001 -- deliberately broad, see above
            print(f"[udp] ERROR handling packet: {e!r} -- raw: {data[:300]!r}", flush=True)


app = Flask(__name__)


@app.route("/api/conditions")
def api_conditions():
    with state_lock:
        tempest_copy = dict(latest_conditions)
        obs_age = (None if last_obs_received_monotonic is None
                   else time.monotonic() - last_obs_received_monotonic)
        nws_available = latest_nws["available"]
        nws_last_polled = latest_nws["last_polled_at"]
        alerts_copy = [
            {
                **alert,
                "needs_alert": (
                    alert["last_transition"] in ("new", "escalated")
                    and not alert["acknowledged"]
                ),
            }
            for alert in tracked_alerts.values()
            if alert["last_transition"] != "expired"
        ]

    # Called OUTSIDE the block above, deliberately -- get_lightning_status()
    # acquires state_lock itself, and this lock is a plain threading.Lock
    # (not reentrant), so calling it while still holding the lock above
    # would deadlock the very first time this endpoint is hit.
    lightning_status = get_lightning_status()

    # "available" is only true while observations are actually still
    # arriving -- not merely because one packet arrived once, some time
    # ago. Strike events share the same UDP source, so this also stands
    # in for "the lightning feed is alive". The last good values are
    # left in place (same stale-data convention as NWS below); the
    # device decides how to present them.
    if obs_age is None or obs_age > TEMPEST_STALE_SEC:
        tempest_copy["available"] = False
    tempest_copy["observation_age_sec"] = None if obs_age is None else round(obs_age)

    return jsonify({
        "tempest": tempest_copy,
        "lightning": lightning_status,
        "nws": {
            "available": nws_available,
            "last_polled_at": nws_last_polled,
            "alerts": alerts_copy,
        },
    })


@app.route("/api/alerts/ack", methods=["POST"])
def api_ack_alert():
    data = request.get_json(silent=True) or {}
    alert_id = data.get("id")
    if not alert_id:
        return jsonify({"error": "missing 'id' in request body"}), 400

    with state_lock:
        if alert_id not in tracked_alerts:
            return jsonify({"error": "unknown alert id"}), 404
        tracked_alerts[alert_id]["acknowledged"] = True
        save_persisted_alerts_locked()

    return jsonify({"status": "ok", "id": alert_id})


@app.route("/healthz")
def healthz():
    return jsonify({"status": "ok"})


if __name__ == "__main__":
    load_persisted_alerts()
    print(f"[config] lightning filter radius = {LIGHTNING_FILTER_RADIUS_MI:g} mi "
          f"(default is 10; set LIGHTNING_FILTER_RADIUS_MI to change)", flush=True)

    listener = threading.Thread(target=udp_listener_thread, daemon=True)
    listener.start()

    nws_thread = threading.Thread(target=nws_poller_thread, daemon=True)
    nws_thread.start()

    print(f"[http] Serving on port {HTTP_PORT}")
    app.run(host="0.0.0.0", port=HTTP_PORT)
