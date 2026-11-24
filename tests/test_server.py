"""2번 오류 응답과 기존 정상 동작의 실제 TCP 검사."""
import os
import socket
import time
import unittest

PORT = int(os.environ.get("SERVER_TEST_PORT", "8080"))
NORMAL = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
REASONS = {200: "OK", 400: "Bad Request", 431: "Request Header Fields Too Large",
           500: "Internal Server Error", 408: "Request Timeout",
           501: "Not Implemented", 505: "HTTP Version Not Supported"}

def expected(status):
    reason = REASONS[status]
    body = "hello world" if status == 200 else reason
    return (f"HTTP/1.1 {status} {reason}\r\n" +
            ("Content-Type: text/html; charset=utf-8\r\n" if status == 200 else "Content-Type: text/plain; charset=utf-8\r\n") +
            f"Content-Length: {len(body.encode())}\r\n"
            f"Connection: close\r\n\r\n{body}").encode()

EXPECTED = expected(200)

def read_all(connection):
    response = bytearray()
    while True:
        try:
            part = connection.recv(4096)
        except ConnectionResetError:
            break
        if not part:
            return bytes(response)
        response.extend(part)
    return bytes(response)

def exchange(request, port=PORT):
    with socket.create_connection(("127.0.0.1", port), timeout=4) as connection:
        try:
            connection.sendall(request)
        except (ConnectionResetError, BrokenPipeError):
            pass
        return read_all(connection)

class ServerTests(unittest.TestCase):
    def check_status(self, request, status):
        self.assertEqual(exchange(request), expected(status))
        self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_repeated_success(self):
        for _ in range(10):
            self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_bad_requests_and_recovery(self):
        cases = [b"GET /\r\n", b"GET  / HTTP/1.1\r\n",
                 b"GET / HTTP/abc\r\n", b"G(ET / HTTP/1.0\r\n",
                 b"GET / HTTP/1.1 extra\r\n", b"GET / HTTP/1.1\r\n\r\n",
                 b"GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n",
                 b"GET / HTTP/1.1\r\nHost : localhost\r\n",
                 b"GET / HTTP/1.1\r\nHost: local host\r\n\r\n",
                 b"GET / HTTP/1.1\r\nHost: local\0host\r\n",
                 b"POST / HTTP/1.1\r\n\r\n"]
        for request in cases:
            with self.subTest(request=request):
                self.check_status(request, 400)

    def test_method_policy(self):
        for method in (b"get", b"CUSTOM"):
            self.check_status(method + b" / HTTP/1.1\r\nHost: localhost\r\n\r\n", 501)
        self.assertEqual(exchange(b"POST / HTTP/1.1\r\nHost: localhost\r\n\r\n"),
                         b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")

    def test_unsupported_version_without_waiting_for_headers(self):
        for version in (b"HTTP/1.0", b"HTTP/2.0", b"HTTP/9.9"):
            self.check_status(b"GET / " + version + b"\r\n", 505)

    def test_header_size_boundary(self):
        prefix = b"GET / HTTP/1.1\r\nHost: localhost\r\nX: "
        for size, status in ((8192, 200), (8193, 431)):
            self.check_status(prefix + b"a" * (size - len(prefix) - 4) + b"\r\n\r\n", status)

    def test_fragments_before_deadline(self):
        with socket.create_connection(("127.0.0.1", PORT), timeout=4) as connection:
            connection.sendall(b"GET /?q=cat HTTP/1.1\r\nHost: local")
            connection.settimeout(0.15)
            with self.assertRaises(socket.timeout):
                connection.recv(1)
            connection.sendall(b"host\r\n\r")
            with self.assertRaises(socket.timeout):
                connection.recv(1)
            connection.sendall(b"\n")
            connection.settimeout(4)
            self.assertEqual(read_all(connection), EXPECTED)

    def test_incomplete_half_close(self):
        with socket.create_connection(("127.0.0.1", PORT), timeout=4) as connection:
            connection.sendall(b"GET / HTTP/1.1\r\nHost: local")
            connection.shutdown(socket.SHUT_WR)
            self.assertEqual(read_all(connection), expected(400))
        self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_empty_and_early_disconnect(self):
        with socket.create_connection(("127.0.0.1", PORT), timeout=4) as connection:
            connection.shutdown(socket.SHUT_WR)
            self.assertEqual(read_all(connection), b"")
        for _ in range(5):
            with socket.create_connection(("127.0.0.1", PORT), timeout=4) as connection:
                connection.sendall(NORMAL)
        self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_conflicting_body_headers(self):
        request = (b"GET / HTTP/1.1\r\nHost: [::1]:8080\r\n"
                   b"Content-Length: abc\r\nTransfer-Encoding: anything\r\n\r\n")
        self.assertEqual(exchange(request), expected(400))

    def test_timeout_idle(self):
        with socket.create_connection(("127.0.0.1", PORT), timeout=14) as connection:
            start = time.monotonic()
            self.assertEqual(read_all(connection), expected(408))
            elapsed = time.monotonic() - start
            self.assertGreaterEqual(elapsed, 9.8)
            self.assertLess(elapsed, 13)
        self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_timeout_trickle_does_not_reset_deadline(self):
        with socket.create_connection(("127.0.0.1", PORT), timeout=14) as connection:
            start = time.monotonic()
            for part in (b"GET", b" /", b" HTTP/1.1", b"\r\nHost:", b" local"):
                connection.sendall(part)
                time.sleep(1.8)
            self.assertEqual(read_all(connection), expected(408))
            self.assertLess(time.monotonic() - start, 13)
        self.assertEqual(exchange(NORMAL), EXPECTED)

    @unittest.skipUnless(os.environ.get("SERVER_ALLOC_FAIL_PORT"), "allocation-failure server not configured")
    def test_allocation_failure_500(self):
        for _ in range(3):
            self.assertEqual(exchange(NORMAL, int(os.environ["SERVER_ALLOC_FAIL_PORT"])), expected(500))

if __name__ == "__main__":
    unittest.main(verbosity=2)
