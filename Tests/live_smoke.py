#!/usr/bin/env python3
"""Live-control check: drives a RUNNING Plugin Analyzer (app or plugin window with
"Allow LLM control (MCP)" enabled) through the MCP server.

Usage:  python3 Tests/live_smoke.py <plugin-analyzer-cli> [real plugin path] [snapshot.png]
Run it locally with exactly one instance open. It loads plugins into chain 2 and removes
them again, leaving chain 1 alone.
"""
import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp_smoke import Client, check, metric  # noqa: E402
import mcp_smoke  # noqa: E402


def payload(result):
    if result["isError"]:
        return {"error": result["content"][0]["text"]}
    return result.get("structuredContent") or json.loads(result["content"][0]["text"])


def main():
    exe = sys.argv[1]
    real_plugin = sys.argv[2] if len(sys.argv) > 2 else None
    snapshot_path = sys.argv[3] if len(sys.argv) > 3 else None
    c = Client(exe)
    c.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "live_smoke", "version": "1"}})
    c.send("notifications/initialized", notify=True)

    print("Discovery")
    instances = payload(c.call("list_instances", {}))["instances"]
    check(len(instances) == 1, f"one running instance: {[(i['id'], i['kind'], i['host'], i.get('track')) for i in instances]}")
    if len(instances) != 1:
        return 1
    inst = instances[0]["id"]
    check(instances[0]["responding"], "instance answers ping")

    print("State")
    st = payload(c.call("instance_state", {}))
    check("chains" in st and len(st["chains"]) == 2, f"state: view_mode {st.get('view_mode')}, "
          f"{sum(len(ch['slots']) for ch in st.get('chains', []))} plugins loaded")

    print("Change the user's setup (chain 2, slot 6)")
    r = payload(c.call("instance_load_plugin", {"instance": inst, "chain": 2, "slot": 6, "plugin": "builtin:polynomial"}))
    check(r.get("name") == "Polynomial", "load builtin:polynomial into chain 2 slot 6: " + json.dumps(r)[:200])
    r = payload(c.call("instance_set_parameters", {"chain": 2, "slot": 6, "params": {"a2": "0", "a3": "0.1"}}))
    check("applied" in r and r["applied"].get("a3", "").startswith("0.1"), "set a3 = 0.1: " + json.dumps(r)[:200])
    params = payload(c.call("instance_parameters", {"chain": 2, "slot": 6, "filter": "a3"}))
    check(len(params.get("parameters", [])) == 1, "parameters filter")

    print("Measure without touching the user's view")
    before = payload(c.call("instance_state", {})).get("view_mode")
    r = payload(c.call("instance_measure", {"mode": "thd", "settings": {"harmonic_level_db": -6.0206}}))
    thd2 = [m["value"] for m in r.get("metrics", []) if m["name"] == "thd_percent" and m["chain"] == "2" and m["channel"] == "L"]
    check(thd2 and abs(thd2[0] - 0.6135) < 0.002, f"chain 2 thd_percent {thd2} (expected 0.6135)")
    after = payload(c.call("instance_state", {})).get("view_mode")
    check(before == after, f"the user's view stayed on {after}")

    print("Bypass and snapshot")
    check("error" not in payload(c.call("instance_set_bypass", {"chain": 2, "slot": 6, "bypass": True})), "bypass on")
    st = payload(c.call("instance_state", {}))
    slot = [s for s in st["chains"][1]["slots"] if s["slot"] == 6]
    check(slot and slot[0]["bypassed"], "state shows the bypass")
    c.call("instance_set_bypass", {"chain": 2, "slot": 6, "bypass": False})

    shot = c.call("instance_snapshot", {})
    image = [x for x in shot["content"] if x["type"] == "image"]
    ok = bool(image) and base64.b64decode(image[0]["data"])[:8] == b"\x89PNG\r\n\x1a\n"
    check(ok, "snapshot is a PNG image content block: " + (shot["content"][-1].get("text", "") if ok else str(shot)[:200]))
    if ok and snapshot_path:
        with open(snapshot_path, "wb") as f:
            f.write(base64.b64decode(image[0]["data"]))

    if real_plugin:
        print("Real plugin: " + real_plugin)
        r = payload(c.call("instance_load_plugin", {"chain": 2, "slot": 5, "plugin": real_plugin}))
        check("error" not in r, "load into chain 2 slot 5: " + json.dumps(r)[:200])
        r = payload(c.call("instance_measure", {"mode": "specs"}))
        g = [m["value"] for m in r.get("metrics", []) if m["name"] == "gain_db" and m["chain"] == "2"]
        check(bool(g), f"specs measured with it in the chain (chain 2 gain_db {g})")
        c.call("instance_remove_plugin", {"chain": 2, "slot": 5})

    print("Clean up and safety")
    check("error" not in payload(c.call("instance_remove_plugin", {"chain": 2, "slot": 6})), "remove chain 2 slot 6")
    r = c.call("instance_load_plugin", {"chain": 2, "slot": 6, "plugin": "/tmp/elsewhere/evil.vst3"})
    check(r["isError"] and "not allowed" in r["content"][0]["text"], "plugins outside the plugin folders are refused")
    r = c.call("instance_state", {"instance": "nope"})
    check(r["isError"] and "list_instances" in r["content"][0]["text"], "unknown instance id is explained")

    c.close()
    print("\nLive smoke test passed." if mcp_smoke.failures == 0 else f"\nLive smoke test FAILED ({mcp_smoke.failures}).")
    return 0 if mcp_smoke.failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
