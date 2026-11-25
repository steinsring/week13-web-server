"""격리된 NET_ADMIN Docker 컨테이너에서만 실제 SYN drop과 복구를 검사한다.

사용: python3 tests/test_syn_drop.py /tmp/server
호스트 방화벽은 변경하지 않으며, 추가한 컨테이너 OUTPUT 규칙만 finally에서 제거한다.
"""
import http.client
import http.server
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time


class Backend(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Length', '2')
        self.end_headers()
        self.wfile.write(b'ok')

    def log_message(self, *_args):
        pass


def main():
    backend = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Backend)
    thread = threading.Thread(target=backend.serve_forever, daemon=True)
    thread.start()
    upstream_port = backend.server_address[1]
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    rule = ['OUTPUT', '-d', '127.0.0.1', '-p', 'tcp', '--dport',
            str(upstream_port), '--syn', '-j', 'DROP']
    installed = False
    proc = None
    evidence = {'rule': rule, 'connect_timeout_seconds': 5}

    def exchange():
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=9)
        start = time.monotonic()
        try:
            connection.request('GET', '/api/probe')
            response = connection.getresponse()
            return response.status, response.read().decode(), time.monotonic() - start
        finally:
            connection.close()

    try:
        with tempfile.TemporaryDirectory() as root:
            pathlib.Path(root, 'index.html').write_text('index')
            command = [sys.argv[1], str(port), root, '2', '64', '100', '1048576',
                       '30', '10', '100', f'http://localhost:{port}', '5', '10',
                       f'http://127.0.0.1:{upstream_port}']
            evidence['server_command'] = command
            proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if not proc.stdout.readline().startswith(b'Listening'):
                raise RuntimeError('server startup failed')
            baseline = exchange()
            assert baseline[:2] == (200, 'ok'), baseline
            evidence['baseline'] = baseline
            # 실제 TCP SYN만 차단한다. 백엔드/리스너는 살아 있고 프런트 접속은 영향 없다.
            subprocess.run(['iptables', '-A', *rule], check=True)
            installed = True
            failed = exchange()
            evidence['drop_result'] = failed
            evidence['packet_counters'] = subprocess.check_output(
                ['iptables', '-L', 'OUTPUT', '-v', '-n', '-x'], text=True)
            assert failed[0] == 504 and 4.5 <= failed[2] <= 8, failed
            subprocess.run(['iptables', '-D', *rule], check=True)
            installed = False
            recovered = exchange()
            evidence['recovery'] = recovered
            assert recovered[:2] == (200, 'ok'), recovered
            # 종료 뒤에는 서비스 프로세스가 정상적으로 정리되는지 확인한다.
            proc.terminate()
            _, stderr = proc.communicate(timeout=12)
            evidence['server_exit'] = proc.returncode
            evidence['server_stderr'] = stderr.decode()
            assert proc.returncode == 0, evidence
            evidence['status'] = 'PASS'
    finally:
        if installed:
            subprocess.run(['iptables', '-D', *rule], check=False)
        if proc is not None and proc.poll() is None:
            proc.terminate()
            proc.communicate(timeout=12)
        backend.shutdown()
        backend.server_close()
        thread.join(timeout=2)
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()
