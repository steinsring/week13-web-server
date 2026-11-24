"""Linux TCP 동시성·포화·기한·종료 검사. 지연은 테스트 빌드에서만 주입한다."""
import concurrent.futures, os, pathlib, signal, socket, subprocess, tempfile, time, unittest
BINARY=os.environ.get('TEST_SERVER',str(pathlib.Path(__file__).resolve().parents[1]/'server'))
class Running:
    def __init__(self,root,workers=2,queue=16,limit=40,delay=0):
        s=socket.socket();s.bind(('127.0.0.1',0));self.port=s.getsockname()[1];s.close()
        env=dict(os.environ,TEST_OPEN_DELAY_MS=str(delay))
        self.proc=subprocess.Popen([BINARY,str(self.port),str(root),str(workers),str(queue),str(limit),'1048576','30','10','1'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env)
        line=self.proc.stdout.readline()
        if not line.startswith(b'Listening'):
            _,err=self.proc.communicate(timeout=3)
            raise RuntimeError(err.decode())
    def connect(self):return socket.create_connection(('127.0.0.1',self.port),timeout=16)
    def stop(self):
        if self.proc.poll() is None:self.proc.terminate()
        out,err=self.proc.communicate(timeout=13)
        if self.proc.returncode!=0:raise AssertionError(f'exit {self.proc.returncode}: {err.decode()}')
        if b'ERROR: AddressSanitizer' in err or b'runtime error:' in err:raise AssertionError(err.decode())
    def request(self,path='/'):
        with self.connect() as s:
            s.sendall(f'GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode())
            return read_response(s)
def read_response(s):
    data=bytearray()
    while True:
        try:p=s.recv(65536)
        except ConnectionResetError:break
        if not p:break
        data.extend(p)
    h,b=bytes(data).split(b'\r\n\r\n',1)
    lines=h.decode().split('\r\n');headers=dict(x.split(': ',1) for x in lines[1:])
    assert len(b)==int(headers['Content-Length'])
    return int(lines[0].split()[1]),b
class ConcurrencyTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.tmp.name)
        (self.root/'index.html').write_bytes(b'index');(self.root/'slow.txt').write_bytes(b'slow')
        self.body=bytes(range(256))*4096;(self.root/'bulk.bin').write_bytes(self.body)
        with open(self.root/'huge.bin','wb') as f:f.truncate(128*1024*1024)
        self.servers=[]
    def server(self,**kw):
        s=Running(self.root,**kw);self.servers.append(s);return s
    def tearDown(self):
        for s in self.servers:s.stop()
        self.tmp.cleanup()
    def test_parallel_files_and_slow_receive(self):
        s=self.server();slow=s.connect();slow.sendall(b'GET / HTTP/1.1\r\nHost:')
        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
            replies=list(pool.map(lambda _:s.request('/bulk.bin'),range(20)))
        for code,body in replies:self.assertEqual((code,body),(200,self.body))
        slow.close();self.assertEqual(s.request(),(200,b'index'))
    def test_queue_full_503(self):
        s=self.server(workers=1,queue=1,delay=1800)
        with concurrent.futures.ThreadPoolExecutor() as pool:
            a=pool.submit(s.request,'/slow.txt');time.sleep(.2)
            b=pool.submit(s.request,'/slow.txt');time.sleep(.2)
            self.assertEqual(s.request()[0],503)
            self.assertEqual(a.result(),(200,b'slow'));self.assertEqual(b.result(),(200,b'slow'))
    def test_queue_deadline_excludes_running_time(self):
        s=self.server(workers=1,queue=1,delay=11500)
        with concurrent.futures.ThreadPoolExecutor() as pool:
            a=pool.submit(s.request,'/slow.txt');time.sleep(.2);start=time.monotonic()
            self.assertEqual(s.request()[0],503)
            self.assertGreater(time.monotonic()-start,9.7)
            self.assertEqual(a.result(),(200,b'slow'))
        self.assertEqual(s.request(),(200,b'index'))
    def test_connection_limit_resume(self):
        s=self.server(limit=1);a=s.connect();a.sendall(b'GET ')
        b=s.connect();b.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n');b.settimeout(.3)
        with self.assertRaises(socket.timeout):b.recv(1)
        a.close();b.settimeout(4);self.assertEqual(read_response(b),(200,b'index'));b.close()
    def test_header_deadline_and_other_connection(self):
        s=self.server();a=s.connect();a.sendall(b'GET / HTTP/1.1\r\nHost:');start=time.monotonic()
        self.assertEqual(s.request(),(200,b'index'))
        for _ in range(4):time.sleep(2);a.sendall(b' ')
        self.assertEqual(read_response(a)[0],408);self.assertLess(time.monotonic()-start,12);a.close()
    def test_disconnect_and_fd_reuse(self):
        s=self.server(workers=2,delay=100)
        for _ in range(40):
            a=s.connect();a.sendall(b'GET /slow.txt HTTP/1.1\r\nHost: localhost\r\n\r\n');a.close()
            code,body=s.request();self.assertIn(code,(200,503))
            if code==200:self.assertEqual(body,b'index')
        time.sleep(.5);self.assertEqual(s.request(),(200,b'index'))
    def test_graceful_finish(self):
        s=self.server(delay=700);a=s.connect();a.sendall(b'GET /slow.txt HTTP/1.1\r\nHost: localhost\r\n\r\n');time.sleep(.15)
        s.proc.send_signal(signal.SIGTERM);self.assertEqual(read_response(a),(200,b'slow'));a.close()
        s.proc.wait(timeout=3);self.assertEqual(s.proc.returncode,0)
    def test_graceful_deadline_and_repeat_signal(self):
        s=self.server(workers=1,delay=20000);a=s.connect();a.sendall(b'GET /slow.txt HTTP/1.1\r\nHost: localhost\r\n\r\n');time.sleep(.2)
        start=time.monotonic();s.proc.send_signal(signal.SIGTERM);time.sleep(2);s.proc.send_signal(signal.SIGTERM)
        s.proc.wait(timeout=11);self.assertLess(time.monotonic()-start,12);self.assertEqual(s.proc.returncode,0);a.close()
    def test_slow_sender_timeout_does_not_block_others(self):
        s=self.server();baseline=len(list(pathlib.Path(f'/proc/{s.proc.pid}/fd').iterdir()));a=s.connect();a.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
        a.sendall(b'GET /huge.bin HTTP/1.1\r\nHost: localhost\r\n\r\n')
        time.sleep(.5);self.assertEqual(s.request(),(200,b'index'))
        time.sleep(11)
        self.assertEqual(len(list(pathlib.Path(f'/proc/{s.proc.pid}/fd').iterdir())),baseline)
        # 송신 진행이 멈춘 연결을 정리한 뒤에도 새 요청은 처리한다.
        self.assertEqual(s.request(),(200,b'index'));a.close()
    def test_invalid_cli(self):
        for values in ([],['8080',str(self.root),'0','1','1'],['8080',str(self.root),'1','0','1'],['8080',str(self.root),'1','1','0'],['8080',str(self.root),'1x','1','1']):
            self.assertNotEqual(subprocess.run([BINARY,*values,'1048576','30','10','1'],capture_output=True,timeout=3).returncode,0)
if __name__=='__main__':unittest.main(verbosity=2)
