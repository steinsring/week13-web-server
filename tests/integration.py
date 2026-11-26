#!/usr/bin/env python3
"""Exercise both proxy entry points using local backends."""
import http.server
import pathlib
import signal
import socket
import socketserver
import subprocess
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Backend(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_body(self, body):
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.send_body(self.server.label)

    def do_POST(self):
        self.send_body(self.rfile.read(int(self.headers["Content-Length"])))


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_ready(port, process):
    for _ in range(100):
        if process.poll() is not None:
            raise RuntimeError(f"server exited with {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.02)
    raise RuntimeError("server did not start")


def request(port, head, tail=b""):
    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        sock.settimeout(8)
        sock.sendall(head)
        if tail:
            time.sleep(0.05)
            sock.sendall(tail)
        response = bytearray()
        while True:
            part = sock.recv(8192)
            if not part:
                break
            response.extend(part)
    header, separator, body = bytes(response).partition(b"\r\n\r\n")
    assert separator, f"missing response headers: {response!r}"
    return header, body


def check(name, condition):
    if not condition:
        raise AssertionError(name)
    print(f"{name}: PASS", flush=True)


def main():
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        www = root / "www"
        www.mkdir()
        (www / "index.html").write_text("home")
        (www / "site.css").write_text("body {}")
        (www / "script.js").write_text("console.log(1)")
        (www / "photo.jpg").write_bytes(b"jpeg")
        (root / "secret").write_text("private")
        (www / "link").symlink_to(root / "secret")

        backends = []
        processes = []
        try:
            for label in (b"one", b"two"):
                server = socketserver.TCPServer(("127.0.0.1", 0), Backend)
                server.label = label
                threading.Thread(target=server.serve_forever, daemon=True).start()
                backends.append(server)

            listen, forward = free_port(), free_port()
            while listen == forward:
                forward = free_port()
            proxy = subprocess.Popen([
                str(ROOT / "mini"), str(listen), str(www),
                "127.0.0.1", str(backends[0].server_address[1]),
                "127.0.0.1", str(backends[1].server_address[1]),
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            processes.append(proxy)
            forward_proxy = subprocess.Popen(
                [str(ROOT / "forward_proxy"), str(forward)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            processes.append(forward_proxy)
            wait_ready(listen, proxy)
            wait_ready(forward, forward_proxy)

            cases = [
                ("static file", b"GET /index.html HTTP/1.1\r\nHost: local\r\n\r\n", b"", b"200", b"home"),
                ("404 video", b"GET /missing HTTP/1.1\r\nHost: local\r\n\r\n", b"", b"404", b"youtube.com/embed/"),
                ("path traversal", b"GET /%2e%2e/secret HTTP/1.1\r\nHost: local\r\n\r\n", b"", b"403", b""),
                ("symlink", b"GET /link HTTP/1.1\r\nHost: local\r\n\r\n", b"", b"403", b""),
                ("missing HTTP version", b"GET /index.html\r\nHost: local\r\n\r\n", b"", b"400", b""),
                ("unsupported transfer encoding", b"POST /upload.txt HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n", b"", b"400", b""),
                ("duplicate length", b"POST /upload.txt HTTP/1.1\r\nHost: local\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx", b"", b"400", b""),
                ("split POST", b"POST /upload.txt HTTP/1.1\r\nHost: local\r\nContent-Length: 5\r\n\r\nhe", b"llo", b"201", b""),
            ]
            for name, head, tail, status, expected in cases:
                headers, body = request(listen, head, tail)
                check(name, status in headers.split(b"\r\n")[0] and expected in body)

            check("POST saved body", (www / "upload.txt").read_bytes() == b"hello")
            headers, _ = request(listen, b"POST /upload.txt HTTP/1.1\r\nHost: local\r\nContent-Length: 1\r\n\r\nx")
            check("POST preserves existing file", b"409" in headers.split(b"\r\n")[0] and
                  (www / "upload.txt").read_bytes() == b"hello")

            for path, content_type in (("site.css", b"text/css"), ("script.js", b"text/javascript"), ("photo.jpg", b"image/jpeg")):
                headers, _ = request(listen, f"GET /{path} HTTP/1.1\r\nHost: local\r\n\r\n".encode())
                check(f"MIME {path}", content_type in headers)

            slow = socket.create_connection(("127.0.0.1", listen), timeout=3)
            try:
                slow.sendall(b"GET /index.html HTTP/1.1\r\n")
                headers, body = request(listen, b"GET /index.html HTTP/1.1\r\nHost: local\r\n\r\n")
                check("concurrent requests", b"200" in headers.split(b"\r\n")[0] and body == b"home")
            finally:
                slow.close()

            for i in range(4):
                _, body = request(listen, b"GET /api/a HTTP/1.1\r\nHost: local\r\n\r\n")
                check(f"round robin {i + 1}", body == (b"one", b"two")[i % 2])

            _, body = request(listen, b"POST /api/a HTTP/1.1\r\nHost: local\r\nContent-Length: 5\r\n\r\nhe", b"llo")
            check("reverse proxy POST", body == b"hello")
            url = f"http://127.0.0.1:{backends[0].server_address[1]}/abc"
            _, body = request(forward, f"GET {url} HTTP/1.1\r\nHost: local\r\n\r\n".encode())
            check("forward proxy GET", body == b"one")

            pending = socket.create_connection(("127.0.0.1", listen), timeout=3)
            pending.sendall(b"GET /index.html HTTP/1.1\r\n")
            proxy.send_signal(signal.SIGTERM)
            proxy.wait(timeout=4)
            pending.close()
            check("graceful shutdown", proxy.returncode == 0)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
            for backend in backends:
                backend.shutdown()
                backend.server_close()


if __name__ == "__main__":
    main()
