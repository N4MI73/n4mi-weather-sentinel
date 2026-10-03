"""NWS_ZONE_ID comes from the environment, with GAC073 as the default
(Session 15). Plain Python, no network: each case imports the server in a
fresh interpreter with a different environment."""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER_DIR = os.path.dirname(HERE)

def zone_for(value):
    env = dict(os.environ)
    env.pop("NWS_ZONE_ID", None)
    if value is not None:
        env["NWS_ZONE_ID"] = value
    env["ALERTS_PERSISTENCE_PATH"] = os.path.join(tempfile.mkdtemp(), "state.json")
    code = ("import sys; sys.path.insert(0, %r); import weather_sentinel_server as s; "
            "print(s.NWS_ZONE_ID); print(s.NWS_URL)") % SERVER_DIR
    out = subprocess.run([sys.executable, "-c", code], env=env,
                         capture_output=True, text=True, check=True).stdout.strip().splitlines()
    return out[-2], out[-1], "\n".join(out[:-2])

checks = fails = 0
def check(name, ok):
    global checks, fails
    checks += 1; fails += (not ok)
    print(("  ok   " if ok else "  FAIL ") + name)

z, url, _ = zone_for(None)
check("not set: GAC073", z == "GAC073")
check("URL uses the zone", url.endswith("/alerts/active/zone/GAC073"))
check("blank: GAC073", zone_for("  ")[0] == "GAC073")
check("another county code: used", zone_for("SCC003")[0] == "SCC003")
check("forecast zone code: used", zone_for("GAZ073")[0] == "GAZ073")
check("lower case and spaces: normalised", zone_for(" gac189 ")[0] == "GAC189")
z, _, log = zone_for("Columbia County")
check("malformed: falls back to GAC073", z == "GAC073")
check("malformed: says so in the log", "IGNORING bad NWS_ZONE_ID" in log)
check("too short: falls back", zone_for("GA073")[0] == "GAC073")
z, url, _ = zone_for("SCC003")
check("URL follows the setting", url.endswith("/alerts/active/zone/SCC003"))

print(f"\n{checks - fails}/{checks} checks passed")
sys.exit(1 if fails else 0)
