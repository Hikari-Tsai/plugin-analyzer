#!/usr/bin/env python3
"""End-to-end check of `plugin-analyzer-cli mcp` speaking the Model Context Protocol over stdio.

Usage:  python3 Tests/mcp_smoke.py <path to plugin-analyzer-cli> [optional real plugin path]
Exit code 0 = all checks passed. Uses only the standard library.
"""
import json
import subprocess
import sys
import time

failures = 0


def check(ok, what):
    global failures
    print(("  PASS  " if ok else "  FAIL  ") + what, flush=True)
    if not ok:
        failures += 1


class Client:
    def __init__(self, exe):
        self.proc = subprocess.Popen([exe, "mcp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.next_id = 1

    def send(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            msg["id"] = self.next_id
            self.next_id += 1
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        if notify:
            return None
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("server closed stdout; stderr:\n" + self.proc.stderr.read())
        reply = json.loads(line)              # every protocol line must be valid JSON
        assert reply.get("id") == msg["id"], f"id mismatch: {reply}"
        return reply

    def call(self, name, arguments):
        return self.send("tools/call", {"name": name, "arguments": arguments})["result"]

    def close(self):
        self.proc.stdin.close()
        try:
            return self.proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return -1


def metric(payload, name, channel="L"):
    for m in payload.get("metrics", []):
        if m["name"] == name and m["channel"] in (channel, "all"):
            return m["value"]
    return None


def main():
    exe = sys.argv[1]
    real_plugin = sys.argv[2] if len(sys.argv) > 2 else None
    c = Client(exe)

    print("Handshake")
    r = c.send("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                              "clientInfo": {"name": "mcp_smoke", "version": "1"}})["result"]
    check(r["protocolVersion"] == "2025-06-18", "protocol version negotiated: " + r["protocolVersion"])
    check(r["serverInfo"]["name"] == "plugin-analyzer" and "tools" in r["capabilities"], "server info and tools capability")
    check(len(r.get("instructions", "")) > 50, "server sends usage instructions")
    c.send("notifications/initialized", notify=True)
    check("result" in c.send("ping"), "ping")

    print("Tools")
    tools = {t["name"]: t for t in c.send("tools/list")["result"]["tools"]}
    expected = {"describe", "list_plugins", "plugin_info", "measure", "list_instances", "instance_state",
                "instance_parameters", "instance_measure", "instance_set_parameters", "instance_load_plugin",
                "instance_remove_plugin", "instance_set_bypass", "instance_set_play", "instance_snapshot"}
    check(set(tools) == expected, f"{len(tools)} tools: " + ", ".join(sorted(tools)))
    check(not tools["instance_remove_plugin"]["annotations"]["readOnlyHint"]
          and tools["instance_remove_plugin"]["annotations"]["destructiveHint"], "tools that change things are annotated")
    modes = tools["measure"]["inputSchema"]["properties"]["mode"]["enum"]
    check("thd" in modes and "specs" in modes and len(modes) == 14, f"measure.mode enum has {len(modes)} modes")

    d = c.call("describe", {})
    check(not d["isError"] and "metrics" in d["structuredContent"], "describe includes the metric glossary")
    check(json.loads(d["content"][0]["text"]) == d["structuredContent"], "text content and structuredContent agree")

    t0 = time.time()
    r = c.call("measure", {"mode": "thd",
                           "settings": {"harmonic_level_db": -6.0206},
                           "chain1": [{"plugin": "builtin:polynomial", "params": {"a2": "0", "a3": "0.1"}}]})
    p = r["structuredContent"]
    check(not r["isError"] and abs(metric(p, "thd_percent") - 0.6135) < 0.001,
          f"measure thd -> thd_percent {metric(p, 'thd_percent')} (expected 0.6135, {time.time() - t0:.2f} s)")
    check("curves" not in p, "metrics only by default (no curves)")
    check(len(r["content"][0]["text"]) < 6000, f"compact response ({len(r['content'][0]['text'])} bytes)")

    r = c.call("measure", {"mode": "linear", "chain1": ["builtin:peakeq"], "max_points": 30})
    curves = r["structuredContent"].get("curves", [])
    check(len(curves) > 0 and all(len(cv.get("points", [])) <= 30 for cv in curves), "curves when max_points > 0")

    r = c.call("measure", {"mode": "specs", "chain1": ["builtin:latency"], "chain2": ["builtin:gain"]})
    chains = {m["chain"] for m in r["structuredContent"]["metrics"]}
    check(chains == {"1", "2"}, "two chains measured side by side")

    print("Safety and errors")
    r = c.call("measure", {"mode": "thd", "chain1": [{"plugin": "/tmp/not-a-plugin-folder/evil.vst3"}]})
    check(r["isError"] and "not allowed" in r["content"][0]["text"], "plugins outside the plugin folders are refused")
    r = c.call("plugin_info", {"plugin": "/etc/passwd"})
    check(r["isError"] and "not allowed" in r["content"][0]["text"], "plugin_info obeys the same rule")
    r = c.call("measure", {"mode": "nope"})
    check(r["isError"] and "Modes:" in r["content"][0]["text"], "tool errors are reported as isError results")
    r = c.send("bogus/method")
    check(r.get("error", {}).get("code") == -32601, "unknown method -> JSON-RPC error -32601")
    c.proc.stdin.write("this is not json\n")
    c.proc.stdin.flush()
    r = json.loads(c.proc.stdout.readline())
    check(r.get("error", {}).get("code") == -32700, "invalid JSON -> parse error -32700, server keeps running")

    print("Resources and discovery")
    res = c.send("resources/list")["result"]["resources"]
    check(any(x["uri"] == "plugin-analyzer://guide" for x in res), "guide resource listed")
    g = c.send("resources/read", {"uri": "plugin-analyzer://guide"})["result"]["contents"][0]
    check("Choosing a mode" in g["text"], "guide resource readable")
    li = c.call("list_instances", {})
    instances = li["structuredContent"]["instances"]
    check(not li["isError"], f"list_instances works ({len(instances)} running)")
    if not instances:
        r = c.call("instance_state", {})
        check(r["isError"] and "Allow LLM control" in r["content"][0]["text"], "instance tools explain how to enable live control")
    check(all("token" not in json.dumps(i) for i in instances), "tokens are never shown to the client")
    lp = c.call("list_plugins", {"filter": ""})
    check(not lp["isError"], f"list_plugins works ({lp['structuredContent']['count']} plugins found)")

    if real_plugin:
        print("Real plugin: " + real_plugin)
        info = c.call("plugin_info", {"plugin": real_plugin})
        check(not info["isError"], "plugin_info: " + info["structuredContent"].get("name", info["content"][0]["text"][:200]))
        r = c.call("measure", {"mode": "specs", "chain1": [{"plugin": real_plugin}]})
        p = r["structuredContent"] if not r["isError"] else {}
        check(not r["isError"], f"specs: gain_db {metric(p, 'gain_db')}, latency {metric(p, 'reported_latency_samples')} samples")

    code = c.close()
    check(code == 0, f"server exits cleanly when stdin closes (exit {code})")
    print("\nMCP smoke test passed." if failures == 0 else f"\nMCP smoke test FAILED ({failures}).")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
