#!/usr/bin/env python3
"""
Tests for close lightning (v1.1.1, 2026-10-04): strikes under 10 miles are
"close", and the close state is held for 30 minutes after the last close
strike (NWS: "Wait 30 minutes after the last rumble of thunder").

Plain Python, no network, NAS or device:

    pip install flask requests
    python server/tests/test_lightning_close.py
"""

import os
import sys
import tempfile
import time
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
# Dan's deployment filters at 20 miles (Portainer); the code default is 10.
os.environ["LIGHTNING_FILTER_RADIUS_MI"] = "20"
os.environ.setdefault("ALERTS_PERSISTENCE_PATH",
                      os.path.join(tempfile.mkdtemp(), "alerts_state.json"))

import weather_sentinel_server as srv  # noqa: E402

client = srv.app.test_client()
passed = 0


def check(name, cond, detail=""):
    global passed
    if not cond:
        raise AssertionError(f"FAIL: {name} {detail}")
    passed += 1
    print(f"  ok  {name}")


def strike(distance_mi, seconds_ago=0):
    km = distance_mi / srv.KM_TO_MI
    srv.handle_evt_strike({"evt": [int(time.time() - seconds_ago), km, 1000]})


def reset():
    with srv.state_lock:
        srv.recent_strikes.clear()
        srv.last_close_strike = None


print("close_status() rules")
now = datetime.now(timezone.utc)
c = srv.close_status(None, now)
check("no close strike: inactive", c["active"] is False and c["last_distance_mi"] is None)
check("reports its settings", c["radius_mi"] == 10.0 and c["hold_minutes"] == 30)
c = srv.close_status({"distance_mi": 6.24, "at": now - timedelta(minutes=14, seconds=30)}, now)
check("14.5 min ago: active", c["active"] is True)
check("minutes_since rounds down", c["minutes_since"] == 14)
check("distance rounded to 0.1", c["last_distance_mi"] == 6.2)
check("epoch given", c["last_strike_epoch"] == int((now - timedelta(minutes=14, seconds=30)).timestamp()))
c = srv.close_status({"distance_mi": 6.0, "at": now - timedelta(minutes=29, seconds=59)}, now)
check("29:59 ago: still active", c["active"] is True)
c = srv.close_status({"distance_mi": 6.0, "at": now - timedelta(minutes=30)}, now)
check("exactly 30 min: over", c["active"] is False)
c = srv.close_status({"distance_mi": 6.0, "at": now + timedelta(minutes=1)}, now)
check("future stamp: ignored", c["active"] is False)

print("Strikes from the Tempest")
reset()
strike(15.0)
lt = srv.get_lightning_status()
check("15 mi strike: lightning active", lt["active"] is True)
check("15 mi strike: not close", lt["close"]["active"] is False)
strike(9.9)
lt = srv.get_lightning_status()
check("9.9 mi strike: close", lt["close"]["active"] is True and lt["close"]["last_distance_mi"] == 9.9)
check("closest in window is 9.9", lt["closest_distance_mi"] == 9.9)
# The Tempest reports whole kilometres, so the 10-mile line falls between
# 16 km (9.9 mi) and 17 km (10.6 mi).
reset(); srv.handle_evt_strike({"evt": [int(time.time()), 16, 1000]})
check("16 km (9.9 mi): close", srv.get_lightning_status()["close"]["active"] is True)
reset(); srv.handle_evt_strike({"evt": [int(time.time()), 17, 1000]})
check("17 km (10.6 mi): not close", srv.get_lightning_status()["close"]["active"] is False)
reset(); strike(4.0); strike(7.5)
check("latest close strike wins (7.5 mi, the most recent)",
      srv.get_lightning_status()["close"]["last_distance_mi"] == 7.5)
reset(); strike(3.0, seconds_ago=20 * 60)
lt = srv.get_lightning_status()
check("close strike 20 min ago: window empty", lt["active"] is False and lt["strike_count_recent"] == 0)
check("...but the close hold is still active", lt["close"]["active"] is True and lt["close"]["minutes_since"] == 20)
reset(); strike(3.0, seconds_ago=31 * 60)
check("close strike 31 min ago: hold over", srv.get_lightning_status()["close"]["active"] is False)
reset(); strike(5.0); strike(4.0, seconds_ago=5 * 60)
check("an older strike arriving late doesn't replace a newer one",
      srv.get_lightning_status()["close"]["last_distance_mi"] == 5.0)
reset(); strike(25.0)
check("beyond the 20-mile filter: not tracked at all",
      srv.get_lightning_status()["active"] is False and srv.get_lightning_status()["close"]["active"] is False)

print("strikes_last_minute (Ham Ops Console, 2026-11-01)")
now = datetime.now(timezone.utc)
check("window is 60 s", srv.LIGHTNING_CLOSE_RATE_WINDOW_SEC == 60)
check("window shorter than the strike window",
      srv.LIGHTNING_CLOSE_RATE_WINDOW_SEC < srv.LIGHTNING_WINDOW_MINUTES * 60)
sl = [{"distance_mi": 3.0, "at": now - timedelta(seconds=10)},
      {"distance_mi": 9.9, "at": now - timedelta(seconds=60)},     # edge: counted
      {"distance_mi": 10.0, "at": now - timedelta(seconds=5)},     # at radius: not close
      {"distance_mi": 15.0, "at": now - timedelta(seconds=5)},     # beyond 10 mi
      {"distance_mi": 4.0, "at": now - timedelta(seconds=61)},     # too old
      {"distance_mi": 4.0, "at": now + timedelta(seconds=5)}]      # future-stamped
check("counts only close strikes in the last 60 s",
      srv.count_close_strikes_last_minute(sl, now) == 2,
      srv.count_close_strikes_last_minute(sl, now))
check("empty list -> 0", srv.count_close_strikes_last_minute([], now) == 0)
c = srv.close_status(None, now)
check("inactive block still carries strikes_last_minute = 0",
      c["strikes_last_minute"] == 0 and type(c["strikes_last_minute"]) is int)
c = srv.close_status(None, now, 3)
check("present even when close is inactive", c["active"] is False and c["strikes_last_minute"] == 3)
reset()
check("quiet: 0", srv.get_lightning_status()["close"]["strikes_last_minute"] == 0)
for _ in range(6):
    strike(4.0, seconds_ago=5)
strike(15.0, seconds_ago=5)           # in the filter, beyond 10 mi
strike(4.0, seconds_ago=120)          # close, but 2 minutes ago
lt = srv.get_lightning_status()
check("six close strikes in the last minute -> 6", lt["close"]["strikes_last_minute"] == 6,
      lt["close"]["strikes_last_minute"])
check("strike_count_recent unchanged in meaning (all 8 in window)", lt["strike_count_recent"] == 8)
reset(); strike(4.0, seconds_ago=90)
lt = srv.get_lightning_status()
check("storm moved on: close still held, rate drops to 0",
      lt["close"]["active"] is True and lt["close"]["strikes_last_minute"] == 0)
reset()

print("/api/conditions carries the close block")
reset(); strike(6.0)
body = client.get("/api/conditions").get_json()
check("lightning.close present", "close" in body["lightning"])
check("lightning.close active", body["lightning"]["close"]["active"] is True)
reset()
body = client.get("/api/conditions").get_json()
check("quiet: close present and inactive", body["lightning"]["close"]["active"] is False)
check("quiet: strikes_last_minute = 0 in the API", body["lightning"]["close"]["strikes_last_minute"] == 0)
reset(); strike(3.0); strike(3.0)
body = client.get("/api/conditions").get_json()
check("API: strikes_last_minute = 2", body["lightning"]["close"]["strikes_last_minute"] == 2)
reset()

print("lightning simulation, step by step")
r = client.post("/api/simulation/start", json={"scenario": "lightning"})
check("scenario starts", r.status_code == 200)
expect = [
    ("clear", False, None, False, None),
    ("distant", True, 15.0, False, None),
    ("close", True, 5.2, True, 0),
    ("frequent close", True, 3.5, True, 0),
    ("hold", False, None, True, 14),
    ("hold over", False, None, False, None),
]
for i, (name, active, closest, close_active, mins) in enumerate(expect):
    if i:
        client.post("/api/simulation/advance")
    lt = client.get("/api/conditions").get_json()["lightning"]
    check(f"step {i} ({name}): lightning.active={active}", lt["active"] is active)
    check(f"step {i} ({name}): closest={closest}", lt["closest_distance_mi"] == closest)
    check(f"step {i} ({name}): close.active={close_active}", lt["close"]["active"] is close_active)
    if mins is not None:
        check(f"step {i} ({name}): minutes_since={mins}", lt["close"]["minutes_since"] == mins)
    want = [0, 0, 1, 6, 0, 0][i]
    check(f"step {i} ({name}): strikes_last_minute={want}", lt["close"]["strikes_last_minute"] == want)
check("6 steps in all", len(srv.LIGHTNING_SCENARIO_STEPS) == 6)
client.post("/api/simulation/stop")
reset()

print(f"\nALL {passed} CHECKS PASSED")
