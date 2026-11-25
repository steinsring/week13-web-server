"""실제 TCP 백엔드로 라우팅·프레이밍·기한·프런트 재사용을 검사한다."""
import json
import os
import pathlib
import socket
import socketserver
import struct
import subprocess
import tempfile
import threading
import time
import unittest

BINARY = os.environ.get('TEST_SERVER', str(pathlib.Path(__file__).resolve().parents[1] / 'server'))

def request(path='/api/echo', method='GET', body=b'', headers=b''):
    return (f'{method} {path} HTTP/1.1\r\nHost: client.invalid\r\nContent-Length: {len(body)}\r\n'.encode()
            + headers + b'\r\n' + body)

class Reader:
    def __init__(self, sock): self.sock, self.pending = sock, b''
    def need(self, count):
        while len(self.pending) < count:
            part = self.sock.recv(65536)
            if not part: raise EOFError(self.pending[:200])
            self.pending += part
    def line(self):
        while b'\r\n' not in self.pending: self.need(len(self.pending)+1)
        line, self.pending = self.pending.split(b'\r\n', 1)
        return line
    def read(self):
        status = int(self.line().split()[1]); pairs=[]
        while True:
            line=self.line()
            if not line: break
            k,v=line.split(b':',1);pairs.append((k.lower(),v.strip()))
        headers=dict(pairs);body=bytearray()
        if status < 200 or status in (204,304): return status,headers,bytes(body)
        if headers.get(b'transfer-encoding')==b'chunked':
            while True:
                n=int(self.line(),16)
                if not n:
                    assert self.line()==b'';break
                self.need(n+2);body.extend(self.pending[:n]);assert self.pending[n:n+2]==b'\r\n'
                self.pending=self.pending[n+2:]
        else:
            n=int(headers[b'content-length']);self.need(n);body.extend(self.pending[:n]);self.pending=self.pending[n:]
        return status,headers,bytes(body)

def all_bytes(sock):
    data=bytearray()
    while True:
        try: part=sock.recv(65536)
        except ConnectionResetError: break
        if not part: break
        data.extend(part)
    return bytes(data)

class Backend(socketserver.ThreadingTCPServer):
    allow_reuse_address=True
    daemon_threads=True
    def __init__(self, number):
        super().__init__(('127.0.0.1',0), Handler);self.number=number;self.records=[]
        self.thread=threading.Thread(target=self.serve_forever,daemon=True);self.thread.start()
    @property
    def origin(self): return f'http://127.0.0.1:{self.server_address[1]}'
    def close(self): self.shutdown();self.server_close();self.thread.join()

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try: self.serve()
        except (BrokenPipeError, ConnectionResetError, OSError): pass
    def serve(self):
        data=b''
        while b'\r\n\r\n' not in data:
            part=self.request.recv(65536)
            if not part:return
            data+=part
        head,body=data.split(b'\r\n\r\n',1)
        lines=head.split(b'\r\n');method,target,_=lines[0].split(b' ')
        headers={k.lower():v.strip() for k,v in (line.split(b':',1) for line in lines[1:])}
        if target==b'/api/no-read': time.sleep(4);return
        length=int(headers.get(b'content-length',b'0'))
        while len(body)<length:
            part=self.request.recv(min(65536,length-len(body)))
            if not part:return
            body+=part
        self.server.records.append((method,target,headers,body,self.client_address[1]))
        def send(x): self.request.sendall(x)
        def fixed(payload,status=b'200 OK',extra=b''):
            send(b'HTTP/1.1 '+status+b'\r\nContent-Length: '+str(len(payload)).encode()+b'\r\n'+extra+b'\r\n'+payload)
        if target.startswith(b'/api/redirect?'):
            kind=target.split(b'?',1)[1]
            values={b'internal':self.server.origin+'/api/login?q=1#frag',
                    b'network':'//'+self.server.origin[7:]+'/api/login',
                    b'external':'https://login.example/login',b'similar':self.server.origin.replace('127.0.0.1','127.0.0.1.evil.example')+'/api',
                    b'relative':'../login?q=1#f'}
            fixed(b'',b'302 Found',b'Location: '+values[kind].encode()+b'\r\n');return
        if target==b'/api/binary':fixed(body);return
        if target==b'/api/delayed':time.sleep(.35);fixed(b'delayed');return
        if target==b'/api/hold':
            fixed(b'end');self.request.settimeout(2)
            self.server.upstream_closed=self.request.recv(1)==b'';return
        if target==b'/api/error':fixed(b'backend failure',b'500 Backend Failure');return
        if target==b'/api/empty':send(b'HTTP/1.1 204 No Content\r\n\r\n');return
        if target==b'/api/notmodified':send(b'HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n');return
        if target==b'/api/eof':send(b'HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\neof-body');return
        if target==b'/api/chunked':
            wire=b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-End\r\n\r\n4; a="x,y"\r\na\0\nb\r\n3\r\nxyz\r\n0\r\nX-End: yes\r\n\r\n'
            for byte in wire:send(bytes([byte]))
            return
        if target.startswith(b'/api/bad?'):
            cases=[b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n',
                   b'HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n',
                   b'HTTP/1.1 200 OK\r\nX: a\x01b\r\n\r\n',b'HTTP/1.1 101 Switching Protocols\r\n\r\n',
                   b'HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n',
                   b'HTTP/1.1 200 OK\r\nLocation: /x\r\nLocation: /y\r\nContent-Length: 0\r\n\r\n',
                   b'HTTP/1.1 200 OK\r\nConnection: Content-Length\r\nContent-Length: bad\r\n\r\n',
                   b'HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n']
            send(cases[int(target.split(b'?')[1])]);return
        if target==b'/api/short':send(b'HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nx');return
        if target==b'/api/badchunk':send(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!\n');return
        if target==b'/api/badtrailer':send(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx\r\n0\r\nContent-Length: 1\r\n\r\n');return
        if target==b'/api/interim-bad':send(b'HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\nHTTP/1.1 nope\r\n\r\n');return
        if target==b'/api/interim':
            send(b'HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n');fixed(b'final');return
        if target==b'/api/timeout':time.sleep(1.6);fixed(b'late');return
        if target==b'/api/partial-timeout':send(b'HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nx');time.sleep(1.6);return
        if target==b'/api/progress':
            send(b'HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n')
            for _ in range(5):time.sleep(.35);send(b'x')
            return
        if target==b'/api/large':
            send(b'HTTP/1.1 200 OK\r\nContent-Length: 8388608\r\n\r\n')
            for _ in range(1024):send(b'x'*8192)
            return
        payload=json.dumps({'backend':self.server.number,'target':target.decode(),
            'headers':{k.decode():v.decode() for k,v in headers.items()}}).encode()
        fixed(payload,extra=b'Connection: close, X-Remove\r\nX-Remove: secret\r\nKeep-Alive: timeout=30\r\nContent-Encoding: identity\r\n')

class ProxyTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=pathlib.Path(self.temp.name)
        (self.root/'index.html').write_bytes(b'index');self.backends=[Backend(1),Backend(2)]
        self.procs=[]
    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:p.terminate()
            _,err=p.communicate(timeout=13)
            self.assertEqual(p.returncode,0,err.decode());self.assertNotIn(b'ERROR: AddressSanitizer',err);self.assertNotIn(b'runtime error:',err)
        for backend in self.backends:backend.close()
        self.temp.cleanup()
    def start(self, backends=None, idle=1):
        with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        self.public=f'http://public.example:{port}'
        args=[BINARY,str(port),str(self.root),'2','16','100','16777216','3','3','100',self.public,'1',str(idle),
              backends or ','.join(b.origin for b in self.backends)]
        p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE);self.procs.append(p)
        self.assertTrue(p.stdout.readline().startswith(b'Listening'));return p,port
    def connect(self,port):return socket.create_connection(('127.0.0.1',port),timeout=8)
    def exchange(self,port,path='/api/echo',**kw):
        with self.connect(port) as sock:sock.sendall(request(path,**kw));return Reader(sock).read()
    def test_round_robin_target_headers_and_new_connections(self):
        _,port=self.start()
        with self.connect(port) as sock:
            reader=Reader(sock)
            for number in [1,2,1,2]:
                sock.sendall(request('/api/echo?x=%2f&empty=',headers=b'Connection: Host, Content-Length, X-Secret\r\nX-Secret: hidden\r\nForwarded: spoof\r\nX-Forwarded-For: spoof\r\nX-Keep: yes\r\n'))
                status,headers,body=reader.read();self.assertEqual(status,200);value=json.loads(body)
                self.assertEqual(value['backend'],number);self.assertEqual(value['target'],'/api/echo?x=%2f&empty=')
                self.assertEqual(value['headers']['host'],self.backends[number-1].origin[7:]);self.assertEqual(value['headers']['connection'],'close')
                self.assertEqual(value['headers']['content-length'],'0');self.assertIn('via',value['headers'])
                for name in ['x-secret','forwarded','x-forwarded-for']:self.assertNotIn(name,value['headers'])
                self.assertEqual(value['headers']['x-keep'],'yes');self.assertNotIn(b'x-remove',headers);self.assertNotIn(b'keep-alive',headers)
                self.assertNotEqual(headers.get(b'connection'),b'close');self.assertEqual(headers[b'content-encoding'],b'identity')
        ports=[r[-1] for b in self.backends for r in b.records];self.assertEqual(len(set(ports)),4)
    def test_route_boundaries_and_binary_post(self):
        _,port=self.start()
        for path in ['/apix','/%61pi/x']:self.assertEqual(self.exchange(port,path)[0],404)
        self.assertEqual(self.exchange(port,'/',method='POST',body=b'local')[0],204)
        payload=bytes(range(256))*4096
        self.assertEqual(self.exchange(port,'/api/binary',method='POST',body=payload)[2],payload)
        self.assertEqual(json.loads(self.exchange(port,'/api?')[2])['target'],'/api?')
    def test_chunked_eof_bodyless_and_keep_alive(self):
        _,port=self.start()
        with self.connect(port) as sock:
            reader=Reader(sock)
            for path,status,body in [('/api/chunked',200,b'a\0\nbxyz'),('/api/eof',200,b'eof-body'),('/api/empty',204,b''),('/api/notmodified',304,b'')]:
                sock.sendall(request(path));reply=reader.read();self.assertEqual(reply[0],status);self.assertEqual(reply[2],body)
                if status==304:break
                self.assertNotEqual(reply[1].get(b'connection'),b'close')
    def test_backend_error_status_is_not_gateway_error(self):
        _,port=self.start();status,_,body=self.exchange(port,'/api/error');self.assertEqual((status,body),(500,b'backend failure'))
    def test_redirect_rewriting(self):
        _,port=self.start()
        for kind in ['internal','network','external','similar','relative']:
            _,headers,_=self.exchange(port,'/api/redirect?'+kind)
            location=headers[b'location'].decode()
            if kind=='internal':self.assertEqual(location,self.public+'/api/login?q=1#frag')
            elif kind=='network':self.assertEqual(location,self.public+'/api/login')
            elif kind=='external':self.assertEqual(location,'https://login.example/login')
            elif kind=='relative':self.assertEqual(location,'../login?q=1#f')
            else:self.assertIn('.evil.example',location)
    def test_bad_response_headers(self):
        _,port=self.start()
        for i in range(8):self.assertEqual(self.exchange(port,'/api/bad?'+str(i))[0],502,i)
    def test_incomplete_and_bad_chunks_close_without_second_response(self):
        _,port=self.start()
        for path in ['/api/short','/api/badchunk','/api/badtrailer','/api/partial-timeout']:
            with self.connect(port) as sock:
                sock.sendall(request(path));data=all_bytes(sock)
                self.assertTrue(data.startswith(b'HTTP/1.1 200 '));self.assertEqual(data.count(b'HTTP/1.1'),1)
                if 'chunk' in path or 'trailer' in path:self.assertFalse(data.endswith(b'0\r\n\r\n'))
    def test_interim_and_final_failure(self):
        _,port=self.start()
        with self.connect(port) as sock:
            reader=Reader(sock);sock.sendall(request('/api/interim-bad'))
            self.assertEqual(reader.read()[0],103);self.assertEqual(reader.read()[0],502)
        with self.connect(port) as sock:
            reader=Reader(sock);sock.sendall(request('/api/interim',method='POST',headers=b'Expect: 100-continue\r\n').replace(b'Content-Length: 0',b'Content-Length: 1'))
            self.assertEqual(reader.read()[0],100);sock.sendall(b'x')
            self.assertEqual(reader.read()[0],103);self.assertEqual(reader.read()[2],b'final')
    def test_response_idle_and_progress(self):
        _,port=self.start();self.assertEqual(self.exchange(port,'/api/timeout')[0],504)
        start=time.monotonic();self.assertEqual(self.exchange(port,'/api/progress')[2],b'xxxxx');self.assertGreater(time.monotonic()-start,1)
    def test_no_retry_and_round_robin_after_failure(self):
        with socket.socket() as probe:probe.bind(('127.0.0.1',0));dead=probe.getsockname()[1]
        _,port=self.start(backends=f'http://127.0.0.1:{dead},'+self.backends[0].origin)
        self.assertEqual(self.exchange(port,method='POST',body=b'once')[0],502);self.assertEqual(len(self.backends[0].records),0)
        self.assertEqual(json.loads(self.exchange(port)[2])['backend'],1)
    def test_backpressure_large_bounded_memory_and_other_connection(self):
        proc,port=self.start()
        def rss():return int(next(line.split()[1] for line in pathlib.Path(f'/proc/{proc.pid}/status').read_text().splitlines() if line.startswith('VmRSS:')))
        baseline=rss()
        with self.connect(port) as slow:
            slow.sendall(request('/api/large'));time.sleep(1.4)
            self.assertLess(rss()-baseline,4096)
            self.assertEqual(self.exchange(port,'/')[2],b'index')
            self.assertEqual(Reader(slow).read()[2],b'x'*8388608)
            slow.sendall(request('/api/empty'));self.assertEqual(Reader(slow).read()[0],204)
    def test_startup_validation(self):
        base=[BINARY,'8080',str(self.root),'1','1','1','100','2','2','100']
        for public,backends in [('http://user@x','http://127.0.0.1:9'),('http://x/path','http://127.0.0.1:9'),('http://x','https://127.0.0.1:9'),('http://x','http://does-not-exist.invalid'),('http://x','http://127.0.0.1:9,')]:
            result=subprocess.run(base+[public,'1','1',backends],capture_output=True,timeout=5);self.assertNotEqual(result.returncode,0)
    def test_upstream_send_timeout_and_new_connection(self):
        _,port=self.start(backends=self.backends[0].origin)
        self.assertEqual(self.exchange(port,'/api/no-read',method='POST',body=b'x'*16777216)[0],504)
        self.assertEqual(self.exchange(port,'/api/empty')[0],204)
    def test_content_length_closes_upstream_without_eof(self):
        _,port=self.start(backends=self.backends[0].origin)
        self.assertEqual(self.exchange(port,'/api/hold')[2],b'end')
        for _ in range(20):
            if getattr(self.backends[0],'upstream_closed',False):break
            time.sleep(.01)
        self.assertTrue(self.backends[0].upstream_closed)
    def test_disconnect_cleanup_and_shutdown_buffered_request(self):
        proc,port=self.start(backends=self.backends[0].origin)
        baseline=len(list(pathlib.Path(f'/proc/{proc.pid}/fd').iterdir()))
        for _ in range(10):
            sock=self.connect(port);sock.sendall(request('/api/delayed'))
            sock.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0));sock.close()
        time.sleep(.5)
        self.assertEqual(len(list(pathlib.Path(f'/proc/{proc.pid}/fd').iterdir())),baseline)
        with self.connect(port) as sock:
            sock.sendall(request('/api/delayed')+request('/'))
            time.sleep(.1);proc.terminate()
            status,headers,body=Reader(sock).read()
            self.assertEqual((status,body),(200,b'delayed'))
            self.assertEqual(headers[b'content-length'],b'7');self.assertEqual(headers[b'connection'],b'close')
            self.assertEqual(sock.recv(1),b'')
        proc.wait(timeout=2)

if __name__=='__main__':unittest.main(verbosity=2)
