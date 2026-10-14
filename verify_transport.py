#!/usr/bin/env python3
"""Bounded C3 SSE regression: GET only, never login/open/reboot/flash a device."""
import argparse
import json
import time
from pathlib import Path
import requests

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="http://tygbot.local")
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--hold-seconds", type=int, default=70)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--expected-version", default="1.1.0")
    args = parser.parse_args()
    assert 1 <= args.cycles <= 30 and 5 <= args.hold_seconds <= 180
    url = args.url.rstrip("/")
    report = {"url": url, "methods_used": ["GET"], "door_requests": 0, "cycles": [], "samples": []}
    live = []
    previous_uptime = 0

    def get(path):
        response = requests.get(url + path, timeout=15)
        response.raise_for_status()
        return response.json()

    def diagnostics():
        nonlocal previous_uptime
        state = get("/diagnostics")
        assert state["web_version"] == args.expected_version
        assert state["uptime_seconds"] + 0.5 >= previous_uptime, "Device restarted during GET-only test"
        previous_uptime = state["uptime_seconds"]
        return state

    def subscribe():
        response = requests.get(url + "/events?diagnostics=1", stream=True, timeout=(5, 8))
        response.raise_for_status(); live.append(response)
        events = {}; kind = ""
        for line in response.iter_lines(chunk_size=1):
            if line.startswith(b"event:"): kind = line[6:].decode().strip()
            elif line.startswith(b"data:"):
                events[kind] = json.loads(line[5:])
                if kind == "diagnostics": break
        assert "snapshot" in events and "diagnostics" in events
        assert isinstance(events["diagnostics"]["transport"]["uptime_seconds"], (int, float))
        return response, events

    def close(response):
        response.close()
        if response in live: live.remove(response)

    def wait_active(expected, timeout=3):
        start = time.monotonic()
        while time.monotonic() - start < timeout:
            state = diagnostics()
            if state["sse_active"] == expected: return time.monotonic() - start
            time.sleep(0.05)
        raise AssertionError(f"Old SSE slot did not recycle promptly: expected {expected}")

    try:
        report["before"] = diagnostics(); report["status_before"] = get("/status")
        base = report["before"]["sse_active"]
        assert base < 2, "Both subscriptions already in use; close a browser tab and rerun, do not evict it"
        for n in range(args.cycles):
            response, events = subscribe()
            close(response)
            released = wait_active(base)
            report["cycles"].append({"index": n + 1, "cleanup_seconds": released,
                                      "float_diagnostic": events["diagnostics"]})
            print(f"Rapid SSE reconnect {n + 1}/{args.cycles}: cleanup {released:.3f}s", flush=True)

        # Fill only currently free slots; never close an existing user's stream.
        owned = [subscribe()[0] for _ in range(2 - base)]
        # Always stream: if a user closes another tab concurrently, this can be
        # accepted as SSE instead of 503. Never wait indefinitely for its body.
        extra = requests.get(url + "/events", stream=True, timeout=(5, 5))
        report["third_subscription_status"] = extra.status_code
        extra.close()
        assert report["third_subscription_status"] == 503, "Subscription count changed concurrently; rerun capacity check"
        close(owned.pop()); wait_active(1)
        replacement, _ = subscribe()
        report["recovered_after_503"] = True
        for response in owned + [replacement]: close(response)
        wait_active(base)

        held, _ = subscribe()
        start = time.monotonic()
        while time.monotonic() - start < args.hold_seconds:
            report["samples"].append(diagnostics())
            print(f"Long stream: {report['samples'][-1]['uptime_seconds']:.1f}s uptime, "
                  f"SSE stack free {report['samples'][-1]['sse_stack_min_free']} bytes", flush=True)
            time.sleep(5)
        close(held); wait_active(base)

        # The same worker and TLS/WSS path, but only an authorised-list GET.
        doors = get("/doors")
        assert doors.get("ok"), "Read-only authorised door query failed"
        report["authorised_door_count"] = len(doors.get("doors", []))
        report["after"] = diagnostics(); report["status_after"] = get("/status")
        assert report["after"]["sse_stack_min_free"] > 2048, "Insufficient measured SSE stack headroom"
        assert report["status_before"]["last_door"] == report["status_after"]["last_door"], "Door history changed during GET-only verification"
        assert report["status_after"]["login"]["ok"], "Login session did not survive"
        report["ok"] = True
    except BaseException as exc:
        report["ok"] = False; report["error"] = type(exc).__name__ + ": " + str(exc)
        raise
    finally:
        for response in live: response.close()
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print("PASS: reconnect, subscription-limit recovery, nested float JSON, monotonic uptime and GET-only door list", flush=True)

if __name__ == "__main__":
    main()
