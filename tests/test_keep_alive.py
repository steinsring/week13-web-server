"""HTTP/1.1 응답 경계를 직접 읽어 동일 소켓의 재사용·순서·회수를 검사한다."""
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time
import unittest

BINARY = os.environ.get('TEST_SERVER', str(pathlib.Path(__file__).resolve().parents[1] / 'server'))


def request(method='GET', path='/', headers=b'', body=b''):
    return f'{method} {path} HTTP/1.1\r\nHost: localhost\r\n'.encode() + headers + b'\r\n' + body


class Reader:
    def __init__(self, sock):
        self.sock = sock
        self.pending = b''

    def read(self):
        while b'\r\n\r\n' not in self.pending:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError('응답 헤더 도중 연결 종료')
            self.pending += chunk
        head, self.pending = self.pending.split(b'\r\n\r\n', 1)
        lines = head.split(b'\r\n')
        status = int(lines[0].split()[1])
        headers = {k.lower(): v.strip().lower() for k, v in (line.split(b':', 1) for line in lines[1:])}
        length = 0 if status in (100, 204) else int(headers[b'content-length'])
        while len(self.pending) < length:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError('응답 본문 도중 연결 종료')
            self.pending += chunk
        body, self.pending = self.pending[:length], self.pending[length:]
        if status in (100, 204):
            assert b'content-length' not in headers and b'transfer-encoding' not in headers
        return status, headers, body


class KeepAliveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        (self.root / 'index.html').write_bytes(b'index')
        (self.root / 'second.txt').write_bytes(b'second')
        (self.root / 'docs').mkdir()
        (self.root / 'docs' / 'index.html').write_bytes(b'docs')
        self.large = bytes(range(256)) * 8192
        (self.root / 'large.bin').write_bytes(self.large)
        self.servers = []

    def tearDown(self):
        for proc in self.servers:
            if proc.poll() is None:
                proc.terminate()
            _, err = proc.communicate(timeout=13)
            self.assertEqual(proc.returncode, 0, err.decode())
            self.assertNotIn(b'ERROR: AddressSanitizer', err)
            self.assertNotIn(b'runtime error:', err)
        self.temp.cleanup()

    def start(self, idle=2, maximum=100, limit=40):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        proc = subprocess.Popen([BINARY, str(port), str(self.root), '2', '16', str(limit),
                                 '1048576', '2', str(idle), str(maximum)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.servers.append(proc)
        self.assertTrue(proc.stdout.readline().startswith(b'Listening'))
        return proc, port

    def connect(self, port):
        return socket.create_connection(('127.0.0.1', port), timeout=12)

    def assert_open(self, reply, status=200):
        self.assertEqual(reply[0], status)
        self.assertNotEqual(reply[1].get(b'connection'), b'close')

    def test_sequential_get_post_and_empty(self):
        _, port = self.start()
        with self.connect(port) as sock:
            reader = Reader(sock)
            for wire, status, body in [(request(), 200, b'index'),
                                      (request('POST', headers=b'Content-Length: 4\r\n', body=b'a\0\nb'), 204, b''),
                                      (request('POST'), 204, b''),
                                      (request(path='/second.txt'), 200, b'second')]:
                sock.sendall(wire)
                reply = reader.read()
                self.assert_open(reply, status)
                self.assertEqual(reply[2], body)
            sock.sendall(request(headers=b'Connection: close\r\n'))
            self.assertEqual(reader.read()[1][b'connection'], b'close')
            self.assertEqual(sock.recv(1), b'')

    def test_pipeline_binary_post_and_partial_next_header(self):
        _, port = self.start()
        following = request(path='/second.txt')
        with self.connect(port) as sock:
            reader = Reader(sock)
            sock.sendall(request('POST', headers=b'Content-Length: 5\r\n', body=b'\0a\r\nZ') + request() + following[:9])
            self.assert_open(reader.read(), 204)
            self.assertEqual(reader.read()[2], b'index')
            sock.sendall(following[9:])
            self.assertEqual(reader.read()[2], b'second')

    def test_many_pipelined_requests_exceed_receive_chunk(self):
        _, port = self.start(maximum=180)
        with self.connect(port) as sock:
            reader = Reader(sock)
            sock.sendall(request('POST') * 179 + request())
            for _ in range(179):
                self.assert_open(reader.read(), 204)
            reply = reader.read()
            self.assertEqual(reply[2], b'index')
            self.assertEqual(reply[1][b'connection'], b'close')
            self.assertEqual(sock.recv(1), b'')

    def test_reusable_403_404_and_redirect(self):
        _, port = self.start()
        with self.connect(port) as sock:
            reader = Reader(sock)
            sock.sendall(request(path='/missing') + request(path='/%2e%2e/out') + request(path='/docs') + request())
            for status in (404, 403, 301, 200):
                self.assert_open(reader.read(), status)

    def test_close_tokens_across_headers(self):
        _, port = self.start()
        with self.connect(port) as sock:
            sock.sendall(request(headers=b'Connection: keep-alive, Upgrade\r\ncOnNeCtIoN: x, ClOsE \r\n') + request())
            reader = Reader(sock)
            self.assertEqual(reader.read()[1][b'connection'], b'close')
            self.assertEqual(reader.pending, b'')
            self.assertEqual(sock.recv(1), b'')

    def test_maximum_counts_100_once(self):
        _, port = self.start(maximum=2)
        with self.connect(port) as sock:
            reader = Reader(sock)
            sock.sendall(request('POST', headers=b'Content-Length: 1\r\nExpect: 100-continue\r\n'))
            self.assertEqual(reader.read()[0], 100)
            sock.sendall(b'x')
            self.assert_open(reader.read(), 204)
            sock.sendall(request() + request(path='/second.txt'))
            reply = reader.read()
            self.assertEqual(reply[1][b'connection'], b'close')
            self.assertEqual(reader.pending, b'')
            self.assertEqual(sock.recv(1), b'')

    def test_invalid_request_closes_and_discards_next(self):
        _, port = self.start()
        with self.connect(port) as sock:
            sock.sendall(request('POST', headers=b'Content-Length: 1,1\r\n') + request())
            reader = Reader(sock)
            reply = reader.read()
            self.assertEqual(reply[0], 400)
            self.assertEqual(reply[1][b'connection'], b'close')
            self.assertEqual(reader.pending, b'')
            self.assertEqual(sock.recv(1), b'')

    def test_idle_timeout_and_next_header_has_separate_deadline(self):
        _, port = self.start(idle=1)
        with self.connect(port) as sock:
            reader = Reader(sock)
            sock.sendall(request())
            self.assert_open(reader.read())
            time.sleep(.55)
            sock.sendall(b'GET / HTTP/1.1\r\nHo')
            time.sleep(.65)
            sock.sendall(b'st: localhost\r\n\r\n')
            self.assert_open(reader.read())
            time.sleep(1.2)
            self.assertEqual(sock.recv(1), b'')

    def test_idle_counts_toward_connection_limit(self):
        _, port = self.start(idle=1, limit=1)
        with self.connect(port) as first, self.connect(port) as second:
            first.sendall(request())
            self.assert_open(Reader(first).read())
            second.sendall(request(headers=b'Connection: close\r\n'))
            second.settimeout(.2)
            with self.assertRaises(socket.timeout):
                second.recv(1)
            second.settimeout(3)
            self.assertEqual(Reader(second).read()[0], 200)
            self.assertEqual(first.recv(1), b'')

    def test_large_response_before_next_and_other_connection(self):
        _, port = self.start()
        with self.connect(port) as slow, self.connect(port) as fast:
            slow.sendall(request(path='/large.bin') + request(path='/second.txt'))
            fast.sendall(request())
            self.assertEqual(Reader(fast).read()[2], b'index')
            time.sleep(.1)
            reader = Reader(slow)
            self.assertEqual(reader.read()[2], self.large)
            self.assertEqual(reader.read()[2], b'second')

    def test_repeated_request_resources(self):
        proc, port = self.start(maximum=500)
        baseline = len(list(pathlib.Path(f'/proc/{proc.pid}/fd').iterdir()))
        with self.connect(port) as sock:
            reader = Reader(sock)
            for i in range(120):
                wire = request('POST', headers=b'Content-Length: 1024\r\n', body=b'x' * 1024) if i % 2 else request()
                sock.sendall(wire)
                self.assert_open(reader.read(), 204 if i % 2 else 200)
            self.assertLessEqual(len(list(pathlib.Path(f'/proc/{proc.pid}/fd').iterdir())), baseline + 1)
        time.sleep(.05)
        self.assertEqual(len(list(pathlib.Path(f'/proc/{proc.pid}/fd').iterdir())), baseline)

    def test_shutdown_idle_and_no_buffered_next_request(self):
        proc, port = self.start(idle=20)
        with self.connect(port) as idle, self.connect(port) as active:
            idle.sendall(request())
            self.assert_open(Reader(idle).read())
            reader = Reader(active)
            active.sendall(request('POST', headers=b'Content-Length: 4\r\n', body=b'a'))
            time.sleep(.05)
            proc.send_signal(signal.SIGTERM)
            time.sleep(.05)
            self.assertEqual(idle.recv(1), b'')
            active.sendall(b'bcd' + request())
            reply = reader.read()
            self.assertEqual(reply[0], 204)
            self.assertEqual(reply[1][b'connection'], b'close')
            self.assertEqual(reader.pending, b'')
            self.assertEqual(active.recv(1), b'')
            proc.wait(timeout=2)

    def test_invalid_keep_alive_cli(self):
        base = [BINARY, '8080', str(self.root), '1', '1', '1', '100', '2']
        for suffix in ([], ['1'], ['0', '1'], ['1', '0'], ['-1', '2'], ['1', 'x'], ['999999999999999999999', '1']):
            self.assertNotEqual(subprocess.run(base + suffix, capture_output=True, timeout=3).returncode, 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
