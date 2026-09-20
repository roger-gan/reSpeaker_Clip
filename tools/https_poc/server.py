"""Bounded HTTPS recording receiver for the Clip experiment; standard library only."""
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import ssl
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_FILE = 8 * 1024 * 1024
ROUTE = re.compile(r"/v1/devices/([A-Za-z0-9_-]{1,32})/recordings/([A-Za-z0-9_-]{1,31})/([1-9][0-9]{0,8})\Z")


class Receiver(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, directory, device, token):
        super().__init__(address, Handler)
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.device, self.token = device, token
        self.commit_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, fmt, *args):
        # Never log URLs, headers, tokens or request payloads.
        pass

    def reply(self, code, payload):
        body = json.dumps(payload, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        self.wfile.write(body)

    def authorized(self):
        values = self.headers.get_all("Authorization", [])
        return len(values) == 1 and hmac.compare_digest(
            values[0].encode(), ("Bearer " + self.server.token).encode()
        )

    def do_GET(self):
        if not self.authorized():
            self.reply(401, {"error": "unauthorized"})
        elif self.path == "/healthz":
            self.reply(200, {"status": "ready"})
        else:
            self.reply(404, {"error": "not_found"})

    def do_PUT(self):
        temp_path = None
        try:
            match = ROUTE.fullmatch(self.path)
            if not self.authorized():
                return self.reply(401, {"error": "unauthorized"})
            if not match or match[1] != self.server.device:
                return self.reply(403, {"error": "invalid_identity_or_path"})
            lengths = self.headers.get_all("Content-Length", [])
            hashes = self.headers.get_all("X-Content-SHA256", [])
            if (self.headers.get("Transfer-Encoding") is not None or len(lengths) != 1
                    or not re.fullmatch(r"[0-9]{1,10}", lengths[0]) or len(hashes) != 1
                    or not re.fullmatch(r"[0-9a-f]{64}", hashes[0])):
                return self.reply(400, {"error": "invalid_headers"})
            size = int(lengths[0])
            if not 0 < size <= MAX_FILE:
                return self.reply(413, {"error": "size_limit"})
            expected = hashes[0]
            # Flatten an unambiguous identity digest, never a user-supplied path.
            object_id = hashlib.sha256("/".join(match.groups()).encode()).hexdigest()
            target = self.server.directory / (object_id + ".bin")
            digest = hashlib.sha256()
            with tempfile.NamedTemporaryFile(dir=self.server.directory, prefix=".incoming-",
                                             delete=False) as output:
                temp_path = Path(output.name)
                remaining = size
                while remaining:
                    block = self.rfile.read(min(16384, remaining))
                    if not block:
                        return self.reply(400, {"error": "truncated_upload"})
                    output.write(block)
                    digest.update(block)
                    remaining -= len(block)
                output.flush()
                os.fsync(output.fileno())
            if digest.hexdigest() != expected:
                return self.reply(422, {"error": "hash_mismatch"})
            with self.server.commit_lock:
                code = 201
                if target.exists():
                    with target.open("rb") as existing:
                        old_digest = hashlib.sha256()
                        for block in iter(lambda: existing.read(16384), b""):
                            old_digest.update(block)
                        old_hash = old_digest.hexdigest()
                    if target.stat().st_size != size or old_hash != expected:
                        return self.reply(409, {"error": "identity_conflict"})
                    code = 200
                else:
                    os.replace(temp_path, target)
                    temp_path = None
                directory_fd = os.open(self.server.directory, os.O_RDONLY)
                try:
                    os.fsync(directory_fd)
                finally:
                    os.close(directory_fd)
            self.reply(code, {"status": "stored", "object_id": object_id,
                              "size": size, "sha256": expected})
        except (OSError, ValueError):
            self.close_connection = True
            try:
                self.reply(500, {"error": "io_failure"})
            except OSError:
                pass
        finally:
            if temp_path is not None:
                temp_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--device", default="clip-test")
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    args = parser.parse_args()
    token = args.token_file.read_text().strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]{32,96}", token):
        parser.error("token must be 32-96 URL-safe characters")
    server = Receiver((args.bind, args.port), args.directory, args.device, token)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(args.cert, args.key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"HTTPS receiver listening on {args.bind}:{args.port}", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
