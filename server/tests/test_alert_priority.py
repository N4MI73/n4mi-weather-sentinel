#!/usr/bin/env python3
"""
Tests for v1.0 item 4 -- alert priority ordering (Session 12).

Runs against the real server module with Flask's test client; no network,
no NAS, no device. Plain asserts, no pytest needed:

    pip install flask requests
    python server/tests/test_alert_priority.py

Covers: sort_alerts_for_device() rules, the real /api/conditions path,
the multi_alert simulation scenario end to end (including ack-driven
steps), and regressions for the existing nws_lifecycle scenario and ack
error handling.
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


def alert(aid, level, needs_alert=False, first_seen="2026-09-26T12:00:00+00:00"):
    return {"id": aid, "level": level, "needs_alert": needs_alert, "first_seen_at": first_seen}


def ids(alerts):
    return [a["id"] for a in alerts]


print("sort_alerts_for_device")
s = srv.sort_alerts_for_device
check("empty list", s([]) == [])
check("level order, nothing sounding",
      ids(s([alert("i", "informational"), alert("w", "warning"), alert("a", "advisory"),
             alert("c", "critical"), alert("h", "watch")])) == ["c", "w", "h", "a", "i"])
check("new Watch jumps an acked Warning",
      ids(s([alert("w", "warning"), alert("h", "watch", needs_alert=True)])) == ["h", "w"])
check("new Advisory does NOT jump (silent tier)",
      ids(s([alert("w", "warning"), alert("a", "advisory", needs_alert=True)])) == ["w", "a"])
check("two sounding: higher level first",
      ids(s([alert("h", "watch", True), alert("c", "critical", True)])) == ["c", "h"])
check("same level: newest first",
      ids(s([alert("old", "warning", first_seen="2026-09-26T10:00:00+00:00"),
             alert("new", "warning", first_seen="2026-09-26T11:00:00+00:00")])) == ["new", "old"])
check("unknown/missing level ranks last, never raises",
      ids(s([{"id": "x"}, alert("a", "advisory"), {"id": "y", "level": "bogus"}]))[0] == "a")
inp = [alert("a", "advisory"), alert("w", "warning")]
s(inp)
check("input list not modified", ids(inp) == ["a", "w"])

print("real /api/conditions path")
with srv.state_lock:
    srv.simulation_state.update(active=False, scenario=None, step=0)
    srv.tracked_alerts.clear()
    for aid, level, transition, acked, seen in [
        ("urn:real:adv", "advisory", "unchanged", True, "2026-09-26T09:00:00+00:00"),
        ("urn:real:warn", "warning", "unchanged", True, "2026-09-26T10:00:00+00:00"),
        ("urn:real:watch", "watch", "new", False, "2026-09-26T11:00:00+00:00"),
        ("urn:real:gone", "critical", "expired", True, "2026-09-26T08:00:00+00:00"),
    ]:
        srv.tracked_alerts[aid] = {"id": aid, "event": aid, "level": level,
                                   "last_transition": transition, "acknowledged": acked,
                                   "first_seen_at": seen, "last_updated_at": seen}
    srv.latest_nws["available"] = True
r = client.get("/api/conditions").get_json()
check("expired alert excluded, new Watch first", ids(r["nws"]["alerts"]) ==
      ["urn:real:watch", "urn:real:warn", "urn:real:adv"], ids(r["nws"]["alerts"]))
check("needs_alert computed for the new Watch", r["nws"]["alerts"][0]["needs_alert"] is True)
check("real ack returns 200", client.post("/api/alerts/ack", json={"id": "urn:real:watch"}).status_code == 200)
r = client.get("/api/conditions").get_json()
check("after ack, Warning back on top", ids(r["nws"]["alerts"])[0] == "urn:real:warn")
check("real ack unknown id -> 404", client.post("/api/alerts/ack", json={"id": "nope"}).status_code == 404)
check("real ack missing id -> 400", client.post("/api/alerts/ack", json={}).status_code == 400)
with srv.state_lock:
    srv.tracked_alerts.clear()

print("multi_alert simulation scenario")
check("start", client.post("/api/simulation/start", json={"scenario": "multi_alert"}).status_code == 200)


def sim():
    return client.get("/api/conditions").get_json()


def step():
    return client.get("/api/simulation/status").get_json()["step"]


r = sim()
a = r["nws"]["alerts"]
check("step 0: simulation flag", r["simulation"] is True)
check("step 0: Warning first despite listed second", ids(a) ==
      [srv._SIM_TSTORM_WARNING_ID, srv._SIM_FLOOD_ADVISORY_ID], ids(a))
check("step 0: Warning needs_alert, level warning", a[0]["needs_alert"] and a[0]["level"] == "warning")
check("step 0: two alerts (+1 more)", len(a) == 2)
check("ack of a non-step id -> 404",
      client.post("/api/alerts/ack", json={"id": srv._SIM_FLOOD_WATCH_ID}).status_code == 404)
check("step unchanged after bad ack", step() == 0)
check("ack the Warning -> 200", client.post("/api/alerts/ack", json={"id": srv._SIM_TSTORM_WARNING_ID}).status_code == 200)
check("ack advanced to step 1", step() == 1)
a = sim()["nws"]["alerts"]
check("step 1: Warning still first, nothing needs_alert",
      a[0]["id"] == srv._SIM_TSTORM_WARNING_ID and not any(x["needs_alert"] for x in a))
check("step 1: ack does not advance a non-ack step",
      client.post("/api/alerts/ack", json={"id": srv._SIM_TSTORM_WARNING_ID}).status_code == 200 and step() == 1)
check("advance to step 2", client.post("/api/simulation/advance").status_code == 200 and step() == 2)
a = sim()["nws"]["alerts"]
check("step 2: new Watch jumps to top", ids(a) ==
      [srv._SIM_FLOOD_WATCH_ID, srv._SIM_TSTORM_WARNING_ID, srv._SIM_FLOOD_ADVISORY_ID], ids(a))
check("step 2: three alerts (+2 more)", len(a) == 3)
check("step 2: Watch needs_alert", a[0]["needs_alert"] is True and a[0]["level"] == "watch")
check("ack the Watch advances to step 3",
      client.post("/api/alerts/ack", json={"id": srv._SIM_FLOOD_WATCH_ID}).status_code == 200 and step() == 3)
a = sim()["nws"]["alerts"]
check("step 3: Warning back on top", ids(a)[0] == srv._SIM_TSTORM_WARNING_ID and len(a) == 3)
client.post("/api/simulation/advance")
a = sim()["nws"]["alerts"]
check("step 4: Watch on top, 2 alerts", ids(a) == [srv._SIM_FLOOD_WATCH_ID, srv._SIM_FLOOD_ADVISORY_ID], ids(a))
client.post("/api/simulation/advance")
check("step 5: no alerts", sim()["nws"]["alerts"] == [])
check("advance past final step -> 400", client.post("/api/simulation/advance").status_code == 400)
client.post("/api/simulation/stop")

print("regressions")
client.post("/api/simulation/start", json={"scenario": "nws_lifecycle"})
a = sim()["nws"]["alerts"]
check("nws_lifecycle step 0 unchanged (single alert, original id)",
      len(a) == 1 and a[0]["id"] == srv.SIMULATION_ALERT_ID)
check("nws_lifecycle ack still advances",
      client.post("/api/alerts/ack", json={"id": srv.SIMULATION_ALERT_ID}).status_code == 200 and step() == 1)
with srv.state_lock:
    srv.tracked_alerts["urn:real:x"] = {"id": "urn:real:x", "event": "Tornado Warning",
                                        "level": "critical", "last_transition": "new",
                                        "acknowledged": False,
                                        "first_seen_at": "2026-09-26T12:00:00+00:00",
                                        "last_updated_at": "2026-09-26T12:00:00+00:00"}
r = sim()
check("real alert still auto-stops simulation", r["simulation"] is False
      and ids(r["nws"]["alerts"]) == ["urn:real:x"])
check("status shows inactive", client.get("/api/simulation/status").get_json()["active"] is False)
check("unknown scenario -> 400", client.post("/api/simulation/start", json={"scenario": "x"}).status_code == 400)
check("multi_alert listed", "multi_alert" in client.post(
    "/api/simulation/start", json={"scenario": "x"}).get_json()["available_scenarios"])

print(f"\nALL {passed} CHECKS PASSED")
