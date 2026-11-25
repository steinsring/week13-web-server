"""POST 명세의 실제 TCP 경계, 오류, 기한 및 동시 수신 검사."""
import concurrent.futures
import os
import pathlib
import socket
import subprocess
import tempfile
import time
import unittest

BINARY = os.environ.get('TEST_SERVER', str(pathlib.Path(__file__).resolve().parents[1] / 'server'))


def request(headers=b'', body=b'', method=b'POST', path=b'/submit'):
    return method + b' ' + path + b' HTTP/1.1\r\nHost: localhost\r\n' + headers + b'\r\n' + body


def read_all(sock):
    data = bytearray()
    while True:
        try:
            chunk = sock.recv(65536)
        except ConnectionResetError:
            break
        if not chunk:
            return bytes(data)
        data.extend(chunk)
    return bytes(data)


class PostBodyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        (root / 'index.html').write_bytes(b'index')
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            self.port = probe.getsockname()[1]
        self.proc = subprocess.Popen(
            [BINARY, str(self.port), str(root), '2', '16', '100', '65536', '2', '10', '1', 'http://localhost:8080', '5', '10', 'http://127.0.0.1:9'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if not self.proc.stdout.readline().startswith(b'Listening'):
            _, err = self.proc.communicate(timeout=3)
            self.fail(err.decode())

    def tearDown(self):
        if self.proc.poll() is None:
            self.proc.terminate()
        _, err = self.proc.communicate(timeout=13)
        self.tmp.cleanup()
        self.assertEqual(self.proc.returncode, 0, err.decode())
        self.assertNotIn(b'ERROR: AddressSanitizer', err)
        self.assertNotIn(b'runtime error:', err)

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=5)

    def exchange(self, data):
        with self.connect() as sock:
            sock.sendall(data)
            return read_all(sock)

    def assert_status(self, data, status):
        self.assertTrue(data.startswith(f'HTTP/1.1 {status} '.encode()), data[:200])

    def assert_no_content(self, data):
        self.assert_status(data, 204)
        header, body = data.split(b'\r\n\r\n', 1)
        self.assertEqual(body, b'')
        self.assertNotIn(b'content-length:', header.lower())
        self.assertNotIn(b'transfer-encoding:', header.lower())
        self.assertIn(b'connection: close', header.lower())

    def test_empty_and_arbitrary_paths(self):
        for path in (b'/', b'/missing', b'/docs?x=1'):
            self.assert_no_content(self.exchange(request(path=path)))
            self.assert_no_content(self.exchange(request(b'Content-Length: 0\r\n', path=path)))

    def test_binary_and_body_at_limit(self):
        body = bytes(range(256)) * 256
        self.assert_no_content(self.exchange(request(b'Content-Length: 65536\r\n', body)))

    def test_every_header_body_boundary(self):
        data = request(b'Content-Length: 5\r\n', b'a\x00\r\nb')
        for boundary in range(1, len(data)):
            with self.subTest(boundary=boundary), self.connect() as sock:
                sock.sendall(data[:boundary])
                time.sleep(.002)
                sock.sendall(data[boundary:])
                self.assert_no_content(read_all(sock))

    def test_length_errors(self):
        cases = [b'Content-Length: ' + value + b'\r\n'
                 for value in (b'', b'abc', b'-1', b'+5', b'5, 5', b'18446744073709551616')]
        cases += [b'Content-Length: 5\r\ncontent-length: 5\r\n',
                  b'Content-Length: 0\r\nTransfer-Encoding: chunked\r\n']
        for headers in cases:
            with self.subTest(headers=headers):
                self.assert_status(self.exchange(request(headers)), 400)
        self.assert_status(self.exchange(request(b'Content-Length: 65537\r\n')), 413)

    def test_transfer_encoding(self):
        for value, status in [(b'chunked', 501), (b'gzip, chunked', 501),
                              (b'gzip', 400), (b'chunked, chunked', 400), (b',', 400)]:
            with self.subTest(value=value):
                self.assert_status(self.exchange(request(b'Transfer-Encoding: ' + value + b'\r\n')), status)

    def test_incomplete_half_close(self):
        with self.connect() as sock:
            sock.sendall(request(b'Content-Length: 5\r\n', b'ab'))
            sock.shutdown(socket.SHUT_WR)
            self.assert_status(read_all(sock), 400)

    def test_body_deadline_is_not_extended_by_progress(self):
        with self.connect() as sock:
            sock.sendall(request(b'Content-Length: 100\r\n', b'a'))
            start = time.monotonic()
            time.sleep(.7)
            sock.sendall(b'b')
            time.sleep(.7)
            sock.sendall(b'c')
            self.assert_status(read_all(sock), 408)
            self.assertLess(time.monotonic() - start, 3.0)

    def test_body_deadline_starts_after_headers(self):
        with self.connect() as sock:
            sock.sendall(b'POST / HTTP/1.1\r\nHost: localhost\r\n')
            time.sleep(2.1)
            sock.sendall(b'Content-Length: 1\r\n\r\nx')
            self.assert_no_content(read_all(sock))

    def test_expect_continue_then_final(self):
        with self.connect() as sock:
            sock.sendall(request(b'Content-Length: 5\r\nExpect: 100-continue\r\n'))
            interim = bytearray()
            while not interim.endswith(b'\r\n\r\n'):
                byte = sock.recv(1)
                self.assertTrue(byte, 'connection closed before 100 Continue')
                interim.extend(byte)
            self.assertEqual(bytes(interim), b'HTTP/1.1 100 Continue\r\n\r\n')
            sock.sendall(b'hello')
            self.assert_no_content(read_all(sock))

    def test_expect_errors_before_and_after_continue(self):
        self.assert_status(self.exchange(request(b'Content-Length: 65537\r\nExpect: 100-continue\r\n')), 413)
        self.assert_status(self.exchange(request(b'Expect: magic\r\n')), 417)
        with self.connect() as sock:
            sock.sendall(request(b'Content-Length: 5\r\nExpect: 100-continue\r\n'))
            reply = read_all(sock)
            self.assertTrue(reply.startswith(b'HTTP/1.1 100 Continue\r\n\r\n'))
            self.assertIn(b'HTTP/1.1 408 ', reply)

    def test_parallel_get_post_and_slow_body(self):
        with self.connect() as slow:
            slow.sendall(request(b'Content-Length: 100\r\n', b'a'))
            inputs = [request(b'Content-Length: 3\r\n', b'abc'), request(method=b'GET', path=b'/')] * 20
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                outputs = list(pool.map(self.exchange, inputs))
            for i, reply in enumerate(outputs):
                if i % 2:
                    self.assert_status(reply, 200)
                    self.assertTrue(reply.endswith(b'index'))
                else:
                    self.assert_no_content(reply)

    def test_get_body_and_extra_request_not_executed(self):
        self.assert_status(self.exchange(request(b'Content-Length: 1\r\n', method=b'GET')), 400)
        data = request(b'Content-Length: 1\r\n', b'x') + request(method=b'GET', path=b'/')
        self.assert_no_content(self.exchange(data))

    def test_invalid_new_cli_values(self):
        base = [BINARY, '8080', self.tmp.name, '1', '1', '1']
        for suffix in ([], ['65536'], ['0', '2'], ['65536', '0'], ['65536', '-1'], ['65536', '999999999999999999999']):
            self.assertNotEqual(subprocess.run(base + suffix + ['10', '1', 'http://localhost:8080', '5', '10', 'http://127.0.0.1:9'], capture_output=True, timeout=3).returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
