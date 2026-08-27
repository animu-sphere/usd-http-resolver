"""Component-owned HTTP resolver probe; run with ost runtime exec.

SPDX-License-Identifier: Apache-2.0
Uses only the packaged resolver, OpenUSD, and a loopback fixture origin.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading


def child(url):
    from pxr import Plug, Usd

    plugin = Plug.Registry().GetPluginWithName("HttpResolver")
    if not plugin or not plugin.Load():
        raise RuntimeError("packaged HttpResolver did not load")
    stage = Usd.Stage.Open(url)
    if not stage or not stage.GetPrimAtPath("/Remote"):
        raise RuntimeError("HTTP stage or relative remote sublayer did not open")
    print(json.dumps({"pluginPath": plugin.path, "opened": True}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--child")
    args = parser.parse_args()
    if args.child:
        child(args.child)
        return

    requests = []
    payloads = {
        "/root.usda": b"#usda 1.0\n( subLayers = [@child.usda@] )\n",
        "/child.usda": b"#usda 1.0\ndef Xform \"Remote\" {}\n",
    }

    class Origin(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_HEAD(self):
            self.respond(False)

        def do_GET(self):
            self.respond(True)

        def respond(self, body):
            data = payloads.get(self.path)
            if data is None:
                self.send_error(404)
                return
            start, end = 0, len(data) - 1
            requested = self.headers.get("Range", "")
            status = 200
            if body and requested:
                import re
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
                if not match:
                    self.send_error(416)
                    return
                start = int(match[1])
                end = min(int(match[2]) if match[2] else end, end)
                if start > end:
                    self.send_error(416)
                    return
                status = 206
            self.send_response(status)
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", '"probe-v1"')
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
            self.end_headers()
            if body:
                self.wfile.write(data[start:end + 1])
            requests.append({"method": self.command, "path": self.path,
                             "range": requested, "status": status,
                             "bytesSent": end - start + 1 if body else 0})

    with tempfile.TemporaryDirectory(prefix="http-resolver-probe-") as cache:
        environment = dict(os.environ)
        environment["USD_HTTP_RESOLVER_PERSISTENT_CACHE_DIR"] = cache
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin) as origin:
            thread = threading.Thread(target=origin.serve_forever, daemon=True)
            thread.start()
            url = f"http://127.0.0.1:{origin.server_port}/root.usda"
            observations = []
            boundaries = []
            try:
                for _ in range(2):
                    before = len(requests)
                    completed = subprocess.run(
                        [sys.executable, str(Path(__file__).resolve()), "--child", url],
                        env=environment, capture_output=True, text=True, timeout=60)
                    if completed.returncode:
                        raise RuntimeError(completed.stdout + completed.stderr)
                    observations.append(json.loads(completed.stdout.splitlines()[-1]))
                    boundaries.append((before, len(requests)))
            finally:
                origin.shutdown()
                thread.join()

    first = requests[boundaries[0][0]:boundaries[0][1]]
    second = requests[boundaries[1][0]:boundaries[1][1]]
    first_bytes = sum(row["bytesSent"] for row in first)
    second_bytes = sum(row["bytesSent"] for row in second)
    if not any(row["range"] and row["status"] == 206 for row in first):
        raise RuntimeError("cold open produced no successful HTTP range request")
    if first_bytes <= 0 or second_bytes != 0:
        raise RuntimeError(
            f"persistent cache miss-to-hit failed: first={first_bytes}, second={second_bytes}")
    if not second or any(row["method"] != "HEAD" for row in second):
        raise RuntimeError("warm open was not validator-only")
    print(json.dumps({
        "probe": "http-resolver",
        "status": "passed",
        "pluginPath": observations[0]["pluginPath"],
        "cold": {"requests": first, "bytesFetched": first_bytes},
        "warm": {"requests": second, "bytesFetched": second_bytes},
        "persistentCacheHit": True,
    }))


if __name__ == "__main__":
    main()
