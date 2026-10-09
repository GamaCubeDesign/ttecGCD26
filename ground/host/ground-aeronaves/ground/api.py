"""API local somente leitura do último snapshot exportado pela ground."""

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .config import Config
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--state", type=Path, default=Config().output)
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/health":
                payload, status = b'{"status":"ok"}', 200
            elif self.path == "/state":
                try:
                    state = json.loads(args.state.read_text())
                    for aircraft in state["aircraft"].values():
                        age = max(
                            0, (time.time_ns() - aircraft["last_received_ns"]) / 1e9
                        )
                        aircraft["reception_age_s"] = round(age, 2)
                        aircraft["tracking_status"] = (
                            "ACTIVE" if age <= 60 else "STALE" if age <= 300 else "LOST"
                        )
                    payload, status = (
                        json.dumps(state, ensure_ascii=False).encode(),
                        200,
                    )
                except FileNotFoundError:
                    payload, status = (
                        b'{"aircraft":{},"status":"waiting_for_data"}',
                        200,
                    )
            else:
                payload, status = b'{"error":"not_found"}', 404
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    with ThreadingHTTPServer(("127.0.0.1", args.port), Handler) as server:
        print(f"API local: http://127.0.0.1:{args.port}/state", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
