#!/usr/bin/env python3
"""Serve a web export with cross-origin isolation headers.

Threaded Godot web exports need COOP/COEP or SharedArrayBuffer is
unavailable and the page fails silently. Upstream may ship an equivalent
helper (platform/web/serve.py); this copy exists so the smoke-test loop
is self-contained under misc/webgpu_scripts/.
Usage: python serve.py [directory] [port]
"""
import functools
import http.server
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8060
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", port), functools.partial(Handler, directory=directory)
    )
    print(f"Serving {directory} at http://127.0.0.1:{port} (COOP/COEP on)")
    server.serve_forever()
