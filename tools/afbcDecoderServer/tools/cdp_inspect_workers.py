#!/usr/bin/env python3
"""CDP helper to inspect Emscripten pthread workers in the YUView page.

Usage:
  python3 cdp_inspect_workers.py list          # list all targets
  python3 cdp_inspect_workers.py stack <idx>   # pause worker <idx> and print its JS stack
  python3 cdp_inspect_workers.py stacks        # pause each worker briefly and print stacks
"""
import json
import sys
import time
import urllib.request

import websocket

CDP_HTTP = "http://127.0.0.1:9222"


def get_ws_url():
    with urllib.request.urlopen(f"{CDP_HTTP}/json/version") as resp:
        return json.load(resp)["webSocketDebuggerUrl"]


class CDP:
    def __init__(self, ws_url):
        self.ws = websocket.create_connection(ws_url, timeout=10)
        self.msg_id = 0
        self.pending = {}

    def send(self, method, params=None, session_id=None):
        self.msg_id += 1
        msg = {"id": self.msg_id, "method": method}
        if params is not None:
            msg["params"] = params
        if session_id:
            msg["sessionId"] = session_id
        self.ws.send(json.dumps(msg))
        while True:
            resp = json.loads(self.ws.recv())
            if resp.get("id") == self.msg_id:
                return resp
            # ignore events

    def close(self):
        self.ws.close()


def list_targets(cdp):
    resp = cdp.send("Target.getTargets")
    return resp["result"]["targetInfos"]


def attach(cdp, target_id):
    resp = cdp.send("Target.attachToTarget", {"targetId": target_id, "flatten": True})
    return resp["result"]["sessionId"]


def get_stack(cdp, session_id):
    # Enable debugger, pause, get stack, resume
    cdp.send("Debugger.enable", session_id=session_id)
    cdp.send("Debugger.pause", session_id=session_id)
    time.sleep(0.3)
    resp = cdp.send("Debugger.getStackTrace", session_id=session_id)
    cdp.send("Debugger.resume", session_id=session_id)
    cdp.send("Debugger.disable", session_id=session_id)
    return resp.get("result", {}).get("stackTrace", {})


def format_stack(stack, max_frames=15):
    if not stack:
        return "  (no stack)"
    lines = []
    for frame in stack.get("callFrames", [])[:max_frames]:
        fn = frame.get("functionName", "(anonymous)")
        url = frame.get("url", "")
        line = frame.get("lineNumber", -1) + 1
        col = frame.get("columnNumber", -1) + 1
        lines.append(f"  {fn} @ {url}:{line}:{col}")
    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    cdp = CDP(get_ws_url())
    try:
        targets = list_targets(cdp)
        workers = [t for t in targets if t["type"] == "worker"]
        pages = [t for t in targets if t["type"] == "page"]

        if cmd == "list":
            print(f"pages: {len(pages)}, workers: {len(workers)}")
            for t in targets:
                print(f"  {t['type']:8s} {t.get('title','')[:40]:40s} {t.get('url','')[:60]}")
            return

        if cmd == "stack":
            idx = int(sys.argv[2]) if len(sys.argv) > 2 else 0
            w = workers[idx]
            sid = attach(cdp, w["targetId"])
            print(f"=== {w['title']} ({w['url']}) ===")
            print(format_stack(get_stack(cdp, sid)))
            return

        if cmd == "stacks":
            for i, w in enumerate(workers):
                sid = attach(cdp, w["targetId"])
                print(f"=== worker {i}: {w['title']} ===")
                print(format_stack(get_stack(cdp, sid)))
                print()
            return
    finally:
        cdp.close()


if __name__ == "__main__":
    main()
