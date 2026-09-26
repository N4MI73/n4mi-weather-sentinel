#!/usr/bin/env python3
"""
Tests for the Alert History endpoint (Session 12): GET /api/alerts/history.

    pip install flask requests
    python server/tests/test_alert_history.py
"""

import os
import sys
import tempfile
from datetime import datetime, timedelta, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
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


def iso(hours_ago):
    return (datetime.now(timezone.utc) - timedelta(hours=hours_ago)).isoformat()


def raw(aid, event, severity="Moderate", instruction="Stay alert."):
    return {"id": aid, "event": event, "severity": severity, "instruction": instruction,
            "headline": None, "expires": None, "description": ""}


def history():
    return client.get("/api/alerts/history").get_json()


def ids(entries):
    return [e["id"] for e in entries]


def reset():
    with srv.state_lock:
        srv.tracked_alerts.clear()
        srv.simulation_state.update(active=False, scenario=None, step=0)


print("empty")
reset()
h = history()
check("200 with empty list", h["alerts"] == [] and h["hours"] == 24 and h["simulation"] is False)

print("real lifecycle through the real poll-processing code")
with srv.state_lock:
    srv.process_nws_alerts_locked([raw("A", "Flood Advisory"), raw("W", "Severe Thunderstorm Warning", "Severe")])
h = history()["alerts"]
check("two active, Warning first (sounding, higher level)", ids(h) == ["W", "A"], ids(h))
check("status active, no ended time", all(e["status"] == "active" and e["ended_epoch"] is None for e in h))
check("first_seen_epoch present", all(isinstance(e["first_seen_epoch"], int) for e in h))
check("level carried", h[0]["level"] == "warning" and h[1]["level"] == "advisory")
with srv.state_lock:
    srv.process_nws_alerts_locked([raw("W", "Severe Thunderstorm Warning", "Severe")])   # A drops out
h = history()["alerts"]
check("expired Advisory kept, listed after active", ids(h) == ["W", "A"] and h[1]["status"] == "expired")
check("expired entry has ended_epoch", isinstance(h[1]["ended_epoch"], int))
check("expired alert NOT served by /api/conditions",
      [a["id"] for a in client.get("/api/conditions").get_json()["nws"]["alerts"]] == ["W"])

print("24-hour retention")
with srv.state_lock:
    srv.tracked_alerts["A"]["last_updated_at"] = iso(23)
    srv.process_nws_alerts_locked([raw("W", "Severe Thunderstorm Warning", "Severe")])
check("expired 23 h ago still listed", "A" in ids(history()["alerts"]))
with srv.state_lock:
    srv.tracked_alerts["A"]["last_updated_at"] = iso(25)
    srv.process_nws_alerts_locked([raw("W", "Severe Thunderstorm Warning", "Severe")])
check("expired 25 h ago pruned", "A" not in ids(history()["alerts"]))

print("ordering of several expired alerts, and the size bound")
reset()
with srv.state_lock:
    for i in range(15):
        srv.tracked_alerts[f"E{i}"] = {"id": f"E{i}", "event": "Flood Advisory", "level": "advisory",
                                       "last_transition": "expired", "acknowledged": True,
                                       "first_seen_at": iso(20), "last_updated_at": iso(15 - i),
                                       "instruction": "x " * 400}
h = history()["alerts"]
check("bounded to 12 entries", len(h) == srv.ALERT_HISTORY_MAX_ALERTS)
check("most recently ended first", ids(h)[:3] == ["E14", "E13", "E12"], ids(h)[:3])
check("instruction bounded", all(len(e["instruction"]) <= srv.ALERT_HISTORY_MAX_INSTRUCTION for e in h))

print("acknowledgement state")
reset()
with srv.state_lock:
    srv.process_nws_alerts_locked([raw("H", "Flood Watch")])
check("new Watch unacknowledged", history()["alerts"][0]["acknowledged"] is False)
client.post("/api/alerts/ack", json={"id": "H"})
check("after ack, acknowledged true", history()["alerts"][0]["acknowledged"] is True)

print("simulation")
reset()
client.post("/api/simulation/start", json={"scenario": "multi_alert"})
h = history()
check("simulation flag set", h["simulation"] is True)
check("step 0: current alerts only, sorted", ids(h["alerts"]) ==
      [srv._SIM_TSTORM_WARNING_ID, srv._SIM_FLOOD_ADVISORY_ID], ids(h["alerts"]))
for _ in range(4):
    client.post("/api/simulation/advance")
h = history()["alerts"]   # step 4: Watch + Advisory active, Warning gone
check("step 4: Warning shown as expired after the actives",
      ids(h) == [srv._SIM_FLOOD_WATCH_ID, srv._SIM_FLOOD_ADVISORY_ID, srv._SIM_TSTORM_WARNING_ID]
      and h[2]["status"] == "expired" and isinstance(h[2]["ended_epoch"], int), ids(h))
client.post("/api/simulation/advance")
h = history()["alerts"]   # step 5: everything expired
check("step 5: all three expired, none duplicated",
      sorted(ids(h)) == sorted([srv._SIM_FLOOD_WATCH_ID, srv._SIM_FLOOD_ADVISORY_ID,
                                srv._SIM_TSTORM_WARNING_ID]) and all(e["status"] == "expired" for e in h))
with srv.state_lock:
    srv.process_nws_alerts_locked([raw("R", "Tornado Warning", "Extreme")])
h = history()
check("real alert ends the simulation and shows real history",
      h["simulation"] is False and ids(h["alerts"]) == ["R"])
check("simulation status now inactive", client.get("/api/simulation/status").get_json()["active"] is False)
with srv.state_lock:
    srv.process_nws_alerts_locked([])
client.post("/api/simulation/start", json={"scenario": "nws_lifecycle"})
check("real EXPIRED alerts don't stop a simulation", history()["simulation"] is True)
reset()

print(f"\nALL {passed} CHECKS PASSED")
