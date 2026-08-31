#!/usr/bin/env python3
"""CDP helper to capture the POST payload sent to /v1/afbc/decode.

Usage:
  python3 cdp_capture_decode.py          # capture one decode request and print payload info
"""
import base64
import json
import sys
import urllib.request
import websocket

CDP_HTTP = "http://127.0.0.1:9222"


def get_ws_url():
    with urllib.request.urlopen(f"{CDP_HTTP}/json/version") as resp:
        return json.load(resp)["webSocketDebuggerUrl"]


class CDP:
    def __init__(self, ws_url):
        self.ws = websocket.create_connection(ws_url, timeout=30)
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

    def send_no_wait(self, method, params=None, session_id=None):
        self.msg_id += 1
        msg = {"id": self.msg_id, "method": method}
        if params is not None:
            msg["params"] = params
        if session_id:
            msg["sessionId"] = session_id
        self.ws.send(json.dumps(msg))

    def recv_event(self):
        resp = json.loads(self.ws.recv())
        if resp.get("method"):
            return resp
        return None

    def close(self):
        self.ws.close()


def main():
    cdp = CDP(get_ws_url())
    # List targets
    resp = cdp.send("Target.getTargets")
    page_target = None
    for t in resp["result"]["targetInfos"]:
        if t["type"] == "page" and "127.0.0.1:8080" in t.get("url", ""):
            page_target = t
            break
    if not page_target:
        print("No YUView page target found", file=sys.stderr)
        return

    session_id = cdp.send("Target.attachToTarget",
                          {"targetId": page_target["id"], "flatten": True})["result"]["sessionId"]
    cdp.send("Network.enable", session_id=session_id)
    cdp.send("Network.setCacheDisabled", {"cacheDisabled": True}, session_id=session_id)

    print(f"Capturing decode POST on {page_target['url']} ...", file=sys.stderr)
    captured = None
    while captured is None:
        evt = cdp.recv_event()
        if not evt:
            continue
        if evt.get("method") == "Network.requestWillBeSent":
            params = evt.get("params", {})
            req = params.get("request", {})
            url = req.get("url", "")
            if "/v1/afbc/decode" in url:
                captured = {
                    "method": req.get("method"),
                    "url": url,
                    "headers": req.get("headers", {}),
                    "hasPostData": "postData" in req,
                }
                if "postData" in req:
                    captured["postDataBase64"] = base64.b64encode(
                        req["postData"].encode("latin1")).decode()
                print(f"Captured decode request: {req.get('method')} {url}", file=sys.stderr)
                break
    print(json.dumps(captured, indent=2))


if __name__ == "__main__":
    main()
