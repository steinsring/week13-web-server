"""실행 중인 서버의 실제 TCP 요청·응답 검사 (Python 표준 라이브러리만 사용)."""
import os
import socket
import unittest

PORT = int(os.environ.get("SERVER_TEST_PORT", "8080"))
EXPECTED = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: text/plain; charset=utf-8\r\n"
    b"Content-Length: 11\r\n"
    b"Connection: close\r\n\r\nhello world"
)
NORMAL = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"


def read_all(connection):
    response = bytearray()
    while True:
        try:
            part = connection.recv(4096)
        except ConnectionResetError:
            break
        if not part:
            break
        response.extend(part)
    return bytes(response)


def exchange(request):
    with socket.create_connection(("127.0.0.1", PORT), timeout=3) as connection:
        try:
            connection.sendall(request)
        except (ConnectionResetError, BrokenPipeError):
            pass
        return read_all(connection)


class ServerTests(unittest.TestCase):
    def test_normal_and_repeated_requests(self):
        for _ in range(10):
            self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_waits_for_complete_headers(self):
        with socket.create_connection(("127.0.0.1", PORT), timeout=3) as connection:
            connection.sendall(b"GET /hello%20world?q=a%20b HTTP/1.1\r\nHost: local")
            connection.settimeout(0.2)
            with self.assertRaises(socket.timeout):
                connection.recv(1)
            connection.sendall(b"host\r\nX: one\r\n\r")
            with self.assertRaises(socket.timeout):
                connection.recv(1)
            connection.sendall(b"\n")
            connection.settimeout(3)
            self.assertEqual(read_all(connection), EXPECTED)

    def test_invalid_requests_then_recovery(self):
        invalid = [
            b"GET /\r\nHost: localhost\r\n\r\n",
            b"GET  / HTTP/1.1\r\nHost: localhost\r\n\r\n",
            b"GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",
            b"GET / HTTP/1.1\r\n\r\n",
            b"GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n",
            b"GET / HTTP/1.1\r\nHost : localhost\r\n\r\n",
            b"GET / HTTP/1.1\r\nHost: local host\r\n\r\n",
            b"GET / HTTP/1.1\r\nHost: localhost:abc\r\n\r\n",
            b"GET / HTTP/1.1\r\nHost: local\0host\r\n\r\n",
        ]
        for request in invalid:
            with self.subTest(request=request):
                self.assertEqual(exchange(request), b"")
                self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_storage_only_headers_and_custom_method(self):
        request = (
            b"CUSTOM /search?q=cat HTTP/1.1\r\n"
            b"Host: [::1]:8080\r\n"
            b"Accept: text/html\r\naccept: application/json\r\n"
            b"Content-Length: abc\r\nContent-Length: 9\r\n"
            b"Transfer-Encoding: anything\r\n\r\n"
        )
        self.assertEqual(exchange(request), EXPECTED)

    def test_more_than_initial_header_capacity(self):
        request = b"GET / HTTP/1.1\r\nHost: localhost\r\n"
        request += b"".join(f"X-{i}: value-{i}\r\n".encode() for i in range(100))
        self.assertEqual(exchange(request + b"\r\n"), EXPECTED)

    def test_header_size_boundary(self):
        prefix = b"GET / HTTP/1.1\r\nHost: localhost\r\nX: "
        for size, expected in ((8192, EXPECTED), (8193, b"")):
            request = prefix + b"a" * (size - len(prefix) - 4) + b"\r\n\r\n"
            self.assertEqual(exchange(request), expected)
        self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_incomplete_client_disconnect(self):
        with socket.create_connection(("127.0.0.1", PORT), timeout=3) as connection:
            connection.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\nX: unfinished")
        self.assertEqual(exchange(NORMAL), EXPECTED)

    def test_early_disconnect_after_headers(self):
        for _ in range(10):
            with socket.create_connection(("127.0.0.1", PORT), timeout=3) as connection:
                connection.sendall(NORMAL)
        self.assertEqual(exchange(NORMAL), EXPECTED)


if __name__ == "__main__":
    unittest.main(verbosity=2)
