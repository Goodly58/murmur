"""Tiny local server for Murmur: static files, no-cache headers so edits show on reload.
Usage: py -3.13 serve.py [port]   (default 8742, localhost only)"""
import http.server
import os
import socketserver
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      '.webmanifest': 'application/manifest+json', '.js': 'text/javascript'}

    def end_headers(self):
        self.send_header('Cache-Control', 'no-cache')
        super().end_headers()

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == '__main__':
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8742
    with Server(('127.0.0.1', port), Handler) as server:
        print(f'Murmur on http://127.0.0.1:{port}/  (Ctrl+C to stop)')
        server.serve_forever()
