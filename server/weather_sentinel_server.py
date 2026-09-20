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
    "rain_this_interval_in": None,
}

# --- Lightning: 10-mile filter + heavy/sporadic classification ---
# Confirmed requirement: only strikes within this radius matter to this
# device at all -- farther strikes are outside scope, not tracked.
LIGHTNING_FILTER_RADIUS_MI = 10.0

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
    obs = msg["obs"][0]
    with state_lock:
        latest_conditions["available"] = True
        latest_conditions["observed_at"] = iso_time(obs[0])
        latest_conditions["wind_mph"] = round(obs[2] * MS_TO_MPH, 1)
        latest_conditions["gust_mph"] = round(obs[3] * MS_TO_MPH, 1)
        latest_conditions["wind_direction"] = degrees_to_compass(obs[4])
        latest_conditions["pressure_inhg_station"] = round(obs[6] * MB_TO_INHG, 2)
        latest_conditions["temperature_f"] = round(c_to_f(obs[7]), 1)
        latest_conditions["humidity_percent"] = round(obs[8])
        latest_conditions["rain_this_interval_in"] = round(obs[12] * MM_TO_IN, 3)


def _prune_recent_strikes_locked():
    """Remove strikes older than the classification window. Caller must
    already hold state_lock."""
    cutoff = datetime.now(timezone.utc) - timedelta(minutes=LIGHTNING_WINDOW_MINUTES)
    recent_strikes[:] = [s for s in recent_strikes if s["at"] >= cutoff]


def handle_evt_strike(msg):
    epoch, distance_km, energy = msg["evt"]
    distance_mi = distance_km * KM_TO_MI

    if distance_mi > LIGHTNING_FILTER_RADIUS_MI:
        # Outside the confirmed 10-mile scope -- not relevant to this
        # device, deliberately not tracked at all.
        return

    strike_time = datetime.fromtimestamp(epoch, tz=timezone.utc)
    with state_lock:
        recent_strikes.append({"distance_mi": distance_mi, "at": strike_time})
        _prune_recent_strikes_locked()


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
        data, addr = sock.recvfrom(4096)
        try:
            msg = json.loads(data.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            continue

        msg_type = msg.get("type")
        if msg_type == "obs_st":
            handle_obs_st(msg)
            print(f"[udp] obs_st received, temp={latest_conditions['temperature_f']}F")
        elif msg_type == "evt_strike":
            handle_evt_strike(msg)
            print(f"[udp] evt_strike received (logged after distance filter/pruning)")


app = Flask(__name__)


@app.route("/api/conditions")
def api_conditions():
    with state_lock:
        tempest_copy = dict(latest_conditions)
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

    listener = threading.Thread(target=udp_listener_thread, daemon=True)
    listener.start()

    nws_thread = threading.Thread(target=nws_poller_thread, daemon=True)
    nws_thread.start()

    print(f"[http] Serving on port {HTTP_PORT}")
    app.run(host="0.0.0.0", port=HTTP_PORT)
