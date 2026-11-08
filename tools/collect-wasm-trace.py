#!/usr/bin/env python3
"""Collect opt-in browser diagnostics on loopback without interacting with the game."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import time
from urllib.parse import urlsplit


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", self.server.origin)
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        super().end_headers()

    def do_GET(self):
        self.send_response(200 if self.path == "/ready" else 404)
        self.end_headers()

    def do_OPTIONS(self):
        self.send_response(204 if self.headers.get("Origin") == self.server.origin else 403)
        self.end_headers()

    def do_POST(self):
        if self.path != "/trace" or self.headers.get("Origin") != self.server.origin:
            self.send_error(403)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 65536:
                raise ValueError("invalid trace size")
            data = json.loads(self.rfile.read(length))
            if not isinstance(data, dict) or data.get("trace") != "browser":
                raise ValueError("invalid trace snapshot")
        except (ValueError, RecursionError):
            self.send_error(400)
            return
        with self.server.output.open("a", encoding="utf-8") as output:
            output.write(json.dumps(data) + "\n")
        self.send_response(204)
        self.end_headers()
        print(f"snapshot={data.get('sample')} backing={data.get('backing')} "
              f"image={data.get('image')} changes={data.get('changes')}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", required=True, help="exact local game origin, including port")
    parser.add_argument("--output", required=True, type=Path, help="new JSONL evidence file")
    parser.add_argument("--seconds", type=int, default=240, help="bounded collector lifetime")
    args = parser.parse_args()
    origin = urlsplit(args.origin)
    if (origin.scheme != "http" or origin.hostname not in ("127.0.0.1", "localhost")
            or origin.username or origin.password or origin.path or origin.query or origin.fragment):
        parser.error("--origin must be an HTTP loopback origin with no path")
    try:
        port = origin.port
    except ValueError:
        parser.error("--origin has an invalid port")
    if port == 0 or args.seconds <= 0:
        parser.error("the origin port and --seconds must be positive")
    with args.output.open("x", encoding="utf-8"):
        pass
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.origin = args.origin
    server.output = args.output
    server.timeout = 1
    print(f"tracePort={server.server_port}", flush=True)
    print(f"ready=http://127.0.0.1:{server.server_port}/ready", flush=True)
    try:
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            server.handle_request()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
