#!/usr/bin/env python3
"""
Tests for nws.alerts[].ends (Ham Ops Console request, 2026-11-16).

NWS's `expires` is when the message is due to be replaced, not when the
hazard ends. The server now passes NWS's `ends` through (or, when that is
null, parameters.eventEndingTime[0]). Additive only: lifecycle tracking
must not change.

Runs against the real server module with Flask's test client; no network,
no NAS, no device. Plain asserts, no pytest needed:

    pip install flask requests
    python server/tests/test_alert_ends.py
"""

import os
import sys
import tempfile

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


# Shaped like the live 2026-11-16 sample: expires long before ends.
EXPIRES = "2026-11-16T04:00:00-08:00"
ENDS = "2026-11-20T04:00:00-08:00"


def props(aid="urn:test:wsw", ends=ENDS, event_ending=None, expires=EXPIRES, **extra):
    p = {"id": aid, "event": "Winter Storm Watch", "severity": "Moderate",
         "headline": "Winter Storm Watch issued", "instruction": "Be prepared.",
         "expires": expires, "ends": ends, "parameters": {}}
    if event_ending is not None:
        p["parameters"]["eventEndingTime"] = event_ending
    p.update(extra)
    return p


print("alert_ends()")
f = srv.alert_ends
check("ends used when present", f(props()) == ENDS)
check("ends wins over eventEndingTime", f(props(event_ending=["2026-11-21T00:00:00-08:00"])) == ENDS)
check("null ends falls back to eventEndingTime[0]", f(props(ends=None, event_ending=[ENDS])) == ENDS)
check("null ends, no eventEndingTime -> None", f(props(ends=None)) is None)
check("empty-string ends falls back", f(props(ends="", event_ending=[ENDS])) == ENDS)
check("empty eventEndingTime list -> None", f(props(ends=None, event_ending=[])) is None)
check("non-string eventEndingTime -> None", f(props(ends=None, event_ending=[123])) is None)
check("parameters missing -> None", f({"ends": None}) is None)
check("parameters not a dict -> None, no raise", f({"ends": None, "parameters": "x"}) is None)
check("ends not a string -> None, no raise", f({"ends": 5}) is None)

print("real /api/conditions path")


def poll(props_list):
    with srv.state_lock:
        srv.latest_nws["available"] = True
        srv.process_nws_alerts_locked(props_list)


def served():
    return {a["id"]: a for a in client.get("/api/conditions").get_json()["nws"]["alerts"]}


with srv.state_lock:
    srv.simulation_state.update(active=False, scenario=None, step=0)
    srv.tracked_alerts.clear()
poll([props(), props(aid="urn:test:noend", ends=None),
      props(aid="urn:test:fallback", ends=None, event_ending=[ENDS])])
a = served()
check("ends served for a new alert", a["urn:test:wsw"]["ends"] == ENDS)
check("expires still served unchanged", a["urn:test:wsw"]["expires"] == EXPIRES)
check("null ends served as null (key present)", "ends" in a["urn:test:noend"]
      and a["urn:test:noend"]["ends"] is None)
check("eventEndingTime fallback served", a["urn:test:fallback"]["ends"] == ENDS)
check("new alert still 'new'", a["urn:test:wsw"]["last_transition"] == "new")

poll([props(), props(aid="urn:test:noend", ends=None),
      props(aid="urn:test:fallback", ends=None, event_ending=[ENDS])])
check("same poll again -> unchanged", served()["urn:test:wsw"]["last_transition"] == "unchanged")

client.post("/api/alerts/ack", json={"id": "urn:test:wsw"})
NEW_ENDS = "2026-11-21T04:00:00-08:00"
poll([props(ends=NEW_ENDS), props(aid="urn:test:noend", ends=None),
      props(aid="urn:test:fallback", ends=None, event_ending=[ENDS])])
a = served()["urn:test:wsw"]
check("changed ends is picked up", a["ends"] == NEW_ENDS)
check("ends change alone stays 'unchanged' (additive only)", a["last_transition"] == "unchanged")
check("acknowledgement kept", a["acknowledged"] is True and a["needs_alert"] is False)

print("record persisted before `ends` existed")
with srv.state_lock:
    srv.tracked_alerts.clear()
    srv.tracked_alerts["urn:test:old"] = {
        "id": "urn:test:old", "event": "Flood Watch", "level": "watch",
        "expires": EXPIRES, "last_transition": "unchanged", "acknowledged": True,
        "first_seen_at": "2026-11-16T10:00:00+00:00",
        "last_updated_at": "2026-11-16T10:00:00+00:00"}
a = served()["urn:test:old"]
check("old record served with ends: null", "ends" in a and a["ends"] is None)
with srv.state_lock:
    srv.tracked_alerts.clear()

print("simulation")
client.post("/api/simulation/start", json={"scenario": "multi_alert"})
r = client.get("/api/conditions").get_json()
check("simulated alerts carry ends", all(x.get("ends") for x in r["nws"]["alerts"]))
check("simulated ends later than expires",
      all(x["ends"] > x["expires"] for x in r["nws"]["alerts"]))
client.post("/api/simulation/stop")

print(f"\nALL {passed} CHECKS PASSED")
