import hashlib
import http.client
from pathlib import Path
import ssl
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
import unittest

from make_credentials import generate
from server import Receiver, MAX_FILE


class ReceiverTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        cert, key = generate(root / "credentials")
        cls.token = (root / "credentials/token").read_text().strip()
        cls.context = ssl.create_default_context(cafile=str(cert))
        # Match the firmware protocol rather than silently testing TLS 1.3.
        cls.context.maximum_version = ssl.TLSVersion.TLSv1_2
        cls.server = Receiver(("127.0.0.1", 0), root / "recordings", "clip-test", cls.token)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(cert, key)
        cls.server_context = tls
        cls.server.socket = tls.wrap_socket(cls.server.socket, server_side=True)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        cls.temp.cleanup()

    def request(self, segment, body=b"test opus bytes", overrides=None, path=None, port=None):
        headers = {"Authorization": "Bearer " + self.token,
                   "X-Content-SHA256": hashlib.sha256(body).hexdigest(),
                   "Content-Length": str(len(body))}
        headers.update(overrides or {})
        conn = http.client.HTTPSConnection("localhost", port or self.server.server_port, context=self.context)
        try:
            conn.request("PUT", path or f"/v1/devices/clip-test/recordings/session/{segment}", body, headers)
            response = conn.getresponse()
            return response.status, response.read()
        finally:
            conn.close()

    def test_store_and_retry(self):
        self.assertEqual(self.request(1)[0], 201)
        self.assertEqual(self.request(1)[0], 200)
        self.assertEqual(self.request(1, b"different")[0], 409)

    def test_auth_and_identity(self):
        self.assertEqual(self.request(2, overrides={"Authorization": "Bearer wrong"})[0], 401)
        self.assertEqual(self.request(2, path="/v1/devices/other/recordings/session/2")[0], 403)

    def test_integrity_and_limits(self):
        self.assertEqual(self.request(3, overrides={"X-Content-SHA256": "0" * 64})[0], 422)
        self.assertEqual(self.request(3, overrides={"Content-Length": str(MAX_FILE + 1)})[0], 413)
        self.assertEqual(self.request(3, overrides={"Transfer-Encoding": "chunked"})[0], 400)

    def test_traversal_and_empty(self):
        self.assertEqual(self.request(4, path="/v1/devices/clip-test/recordings/../4")[0], 403)
        self.assertEqual(self.request(4, b"")[0], 413)

    def test_large_stream(self):
        body = bytes(range(256)) * (MAX_FILE // 256)
        self.assertEqual(self.request(5, body)[0], 201)
        self.assertEqual(self.request(5, body)[0], 200)
        self.assertEqual(list(self.server.directory.glob(".incoming-*")), [])

    def test_untrusted_ca(self):
        conn = http.client.HTTPSConnection("localhost", self.server.server_port)
        try:
            with self.assertRaises(ssl.SSLCertVerificationError):
                conn.request("GET", "/healthz")
        finally:
            conn.close()

    def test_wrong_hostname(self):
        conn = http.client.HTTPSConnection("127.0.0.1", self.server.server_port, context=self.context)
        try:
            with self.assertRaises(ssl.SSLCertVerificationError):
                conn.request("GET", "/healthz")
        finally:
            conn.close()

    def test_concurrent_retry(self):
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: self.request(6)[0], range(2)))
        self.assertEqual(sorted(results), [200, 201])

    def test_reopened_receiver(self):
        self.assertEqual(self.request(7)[0], 201)
        # A fresh receiver has no in-memory knowledge of the previous upload.
        other = Receiver(("127.0.0.1", 0), self.server.directory, "clip-test", self.token)
        other.socket = self.server_context.wrap_socket(other.socket, server_side=True)
        thread = threading.Thread(target=other.serve_forever, daemon=True)
        thread.start()
        try:
            self.assertEqual(self.request(7, port=other.server_port)[0], 200)
        finally:
            other.shutdown()
            other.server_close()
            thread.join()

    def test_duplicate_content_length(self):
        conn = http.client.HTTPSConnection("localhost", self.server.server_port, context=self.context)
        try:
            conn.putrequest("PUT", "/v1/devices/clip-test/recordings/session/8")
            conn.putheader("Authorization", "Bearer " + self.token)
            conn.putheader("Content-Length", "1")
            conn.putheader("Content-Length", "2")
            conn.putheader("X-Content-SHA256", "0" * 64)
            conn.endheaders()
            response = conn.getresponse()
            self.assertEqual(response.status, 400)
            response.read()
        finally:
            conn.close()

    def test_interrupted_upload(self):
        conn = http.client.HTTPSConnection("localhost", self.server.server_port, context=self.context)
        conn.connect()
        headers = ("PUT /v1/devices/clip-test/recordings/session/9 HTTP/1.1\r\n"
                   "Host: localhost\r\nAuthorization: Bearer " + self.token +
                   "\r\nContent-Length: 10000\r\nX-Content-SHA256: " + "0" * 64 + "\r\n\r\n")
        conn.sock.sendall(headers.encode() + b"partial")
        conn.close()
        # A successful new request proves the receiver can continue serving.
        self.assertEqual(self.request(9)[0], 201)
        end = time.monotonic() + 2
        while list(self.server.directory.glob(".incoming-*")) and time.monotonic() < end:
            time.sleep(0.01)
        self.assertEqual(list(self.server.directory.glob(".incoming-*")), [])


if __name__ == "__main__":
    unittest.main()
