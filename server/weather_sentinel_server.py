#!/usr/bin/env python3
"""
Weather Sentinel -- backend server, early build.

Combines the confirmed-working Tempest UDP listener (see
tempest_udp_test.py) and the confirmed-working NWS poller (see
nws_alert_test.py) into one long-running service with an HTTP endpoint,
so the device will eventually be able to fetch current conditions and
alert status over the network.

STATUS: general conditions + raw active-alert polling. NOT yet included
(each a separate, deliberately deferred next step):
  - Lightning 10-mile filtering + heavy/sporadic classification
    (this file stores only the single most recent raw strike, unfiltered)
  - Daily rain accumulation, pressure trend, feels-like temperature,
    sea-level pressure adjustment (all deferred earlier)
  - Alert LIFECYCLE tracking (new/updated/escalated/canceled/expired,
    acknowledgement state) -- this file reports NWS's raw active-alert
    list every poll, with no memory of what changed since last time
  - Persistence across restarts
  - Distance/geographic filtering beyond the single fixed zone (GAC073)

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
from datetime import datetime, timezone
from flask import Flask, jsonify
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
latest_strike = {
    "detected": False,
    "detected_at": None,
    "distance_mi": None,
    # NOTE: raw, unfiltered -- does not yet apply the 10-mile cutoff or
    # heavy/sporadic classification. That's separate, deliberately
    # deferred work.
}
latest_nws = {
    # "available" is true ONLY after a successful poll -- per the
    # project's own guardrail, an empty alert list must never be shown
    # unless it came from a real, fresh, successful retrieval. A failed
    # poll leaves available=False rather than silently keeping stale
    # data marked as current.
    "available": False,
    "last_polled_at": None,
    "alerts": [],  # only meaningful when available is True
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


def handle_evt_strike(msg):
    epoch, distance_km, energy = msg["evt"]
    with state_lock:
        latest_strike["detected"] = True
        latest_strike["detected_at"] = iso_time(epoch)
        latest_strike["distance_mi"] = round(distance_km * KM_TO_MI, 1)


def poll_nws_once():
    try:
        response = requests.get(NWS_URL, headers=NWS_HEADERS, timeout=10)
        response.raise_for_status()
        data = response.json()
        features = data.get("features", [])

        alerts = []
        for feature in features:
            props = feature.get("properties", {})
            alerts.append({
                "id": props.get("id"),
                "event": props.get("event"),
                "severity": props.get("severity"),
                "urgency": props.get("urgency"),
                "certainty": props.get("certainty"),
                "onset": props.get("onset"),
                "expires": props.get("expires"),
                "headline": props.get("headline"),
                "instruction": props.get("instruction"),
            })

        with state_lock:
            latest_nws["available"] = True
            latest_nws["last_polled_at"] = datetime.now(timezone.utc).isoformat()
            latest_nws["alerts"] = alerts

        print(f"[nws] Poll succeeded -- {len(alerts)} active alert(s)")

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
            print(f"[udp] evt_strike received, distance={latest_strike['distance_mi']}mi")


app = Flask(__name__)


@app.route("/api/conditions")
def api_conditions():
    with state_lock:
        return jsonify({
            "tempest": dict(latest_conditions),
            "last_strike": dict(latest_strike),
            "nws": {
                "available": latest_nws["available"],
                "last_polled_at": latest_nws["last_polled_at"],
                "alerts": list(latest_nws["alerts"]),
            },
        })


@app.route("/healthz")
def healthz():
    return jsonify({"status": "ok"})


if __name__ == "__main__":
    listener = threading.Thread(target=udp_listener_thread, daemon=True)
    listener.start()

    nws_thread = threading.Thread(target=nws_poller_thread, daemon=True)
    nws_thread.start()

    print(f"[http] Serving on port {HTTP_PORT}")
    app.run(host="0.0.0.0", port=HTTP_PORT)
