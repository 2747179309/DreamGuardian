#!/usr/bin/env python3
import argparse
import os
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


DEFAULT_UPSTREAM = "https://api.openai.com/v1/audio/transcriptions"


class AsrBridgeHandler(BaseHTTPRequestHandler):
    upstream_url = DEFAULT_UPSTREAM
    api_key = ""

    def log_message(self, fmt, *args):
        sys.stdout.write("ASR bridge: " + (fmt % args) + "\n")
        sys.stdout.flush()

    def do_GET(self):
        if self.path == "/health":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"ok\n")
            return
        self.send_error(404, "not found")

    def do_POST(self):
        if self.path not in ("/v1/audio/transcriptions", "/audio/transcriptions"):
            self.send_error(404, "not found")
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        content_type = self.headers.get("Content-Type", "")
        auth = self.headers.get("Authorization", "")
        if self.api_key:
            auth = "Bearer " + self.api_key

        headers = {
            "Content-Type": content_type,
            "Authorization": auth,
            "User-Agent": "dreamguardian-asr-bridge/1.0",
        }
        req = urllib.request.Request(
            self.upstream_url,
            data=body,
            headers=headers,
            method="POST",
        )
        self.log_message("forward bytes=%u upstream=%s", len(body), self.upstream_url)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                payload = resp.read()
                status = resp.status
                resp_type = resp.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as exc:
            payload = exc.read()
            status = exc.code
            resp_type = exc.headers.get("Content-Type", "application/json")
            self.log_message("upstream http=%u body=%s", status, payload[:240].decode("utf-8", "replace"))
        except Exception as exc:
            payload = ('{"error":"bridge_failed","detail":"%s"}' % str(exc).replace('"', "'")).encode("utf-8")
            status = 502
            resp_type = "application/json"
            self.log_message("upstream failed: %s", exc)

        self.send_response(status)
        self.send_header("Content-Type", resp_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main():
    parser = argparse.ArgumentParser(description="OpenAI-compatible ASR bridge for DreamGuardian ESP32.")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8008)
    parser.add_argument("--upstream", default=DEFAULT_UPSTREAM)
    parser.add_argument("--api-key", default=os.environ.get("OPENAI_API_KEY", ""))
    args = parser.parse_args()

    AsrBridgeHandler.upstream_url = args.upstream
    AsrBridgeHandler.api_key = args.api_key
    server = ThreadingHTTPServer((args.host, args.port), AsrBridgeHandler)
    print("ASR bridge listening on http://%s:%u/v1" % (args.host, args.port))
    print("Upstream: %s" % args.upstream)
    print("API key source: %s" % ("OPENAI_API_KEY/--api-key" if args.api_key else "ESP Authorization header"))
    server.serve_forever()


if __name__ == "__main__":
    main()
