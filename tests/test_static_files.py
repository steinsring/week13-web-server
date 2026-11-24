"""Linux에서 임시 파일 루트와 서버를 만들어 검증한다."""
import os, pathlib, socket, subprocess, tempfile, time, unittest
SERVER = os.environ.get('TEST_SERVER', str(pathlib.Path(__file__).resolve().parents[1] / 'server'))

class StaticFilesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.base = pathlib.Path(cls.temp.name)
        cls.root = cls.base / 'public'; cls.root.mkdir()
        (cls.root/'index.html').write_bytes(b'hello world')
        (cls.root/'docs').mkdir(); (cls.root/'docs'/'index.html').write_bytes(b'<h1>docs</h1>')
        (cls.root/'emptydir').mkdir()
        (cls.root/'hello world.txt').write_bytes(b'space')
        (cls.root/'hello%20world.txt').write_bytes(b'literal percent')
        (cls.root/'a+b.txt').write_bytes(b'plus')
        cls.payload = bytes(range(256))*8192
        (cls.root/'binary.xyz').write_bytes(cls.payload)
        (cls.root/'empty').write_bytes(b'')
        (cls.base/'secret').write_bytes(b'OUTSIDE SECRET')
        (cls.root/'link').symlink_to(cls.base/'secret')
        (cls.root/'inside').symlink_to(cls.root/'index.html')
        (cls.root/'dirlink').symlink_to(cls.root/'docs', target_is_directory=True)
        (cls.root/'linkedindex').mkdir(); (cls.root/'linkedindex'/'index.html').symlink_to(cls.root/'index.html')
        (cls.root/'denied').write_bytes(b'private'); (cls.root/'denied').chmod(0)
        (cls.root/'blocked').mkdir(); (cls.root/'blocked').chmod(0)
        os.mkfifo(cls.root/'fifo')
        cls.sock=socket.socket(socket.AF_UNIX); cls.sock.bind(str(cls.root/'socket'))
        for ext in ('html','htm','CSS','js','txt','png','jpg','jpeg','gif','svg','ico'):
            (cls.root/('sample.'+ext)).write_bytes(b'abc\x00def')
        s=socket.socket(); s.bind(('127.0.0.1',0)); cls.port=s.getsockname()[1]; s.close()
        cls.proc=subprocess.Popen([SERVER,str(cls.port),str(cls.root),'4','64','100','1048576','30','10','1'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        if cls.proc.stdout.readline()!=f'Listening on port {cls.port}\n'.encode():
            raise RuntimeError(cls.proc.stderr.read().decode())
    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate(); cls.proc.communicate(timeout=4)
        cls.sock.close(); (cls.root/'denied').chmod(0o600); (cls.root/'blocked').chmod(0o700)
        cls.temp.cleanup()
    def request(self,path,method='GET'):
        with socket.create_connection(('127.0.0.1',self.port),timeout=3) as s:
            s.sendall(f'{method} {path} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode())
            data=b''
            while True:
                try: part=s.recv(65536)
                except ConnectionResetError: break
                if not part: break
                data+=part
        head,body=data.split(b'\r\n\r\n',1)
        lines=head.decode().split('\r\n')
        code=int(lines[0].split()[1]); headers=dict(line.split(': ',1) for line in lines[1:])
        if code == 204:
            self.assertNotIn('Content-Length', headers)
            self.assertEqual(body, b'')
        else:
            self.assertEqual(int(headers['Content-Length']),len(body))
        self.assertEqual(headers['Connection'],'close')
        return code,headers,body
    def test_index(self):
        self.assertEqual(self.request('/')[2],b'hello world')
        self.assertEqual(self.request('/docs/')[2],b'<h1>docs</h1>')
    def test_redirect_and_query(self):
        c,h,b=self.request('/docs?q=a%20b'); self.assertEqual(c,301)
        self.assertEqual(h['Location'],'/docs/?q=a%20b'); self.assertEqual(b,b'')
        self.assertEqual(self.request('//docs')[1]['Location'],'/docs/')
    def test_encoded_slash(self):
        self.assertEqual(self.request('/docs%2f')[2],b'<h1>docs</h1>')
    def test_binary_and_empty(self):
        c,h,b=self.request('/binary.xyz'); self.assertEqual(c,200)
        self.assertEqual(h['Content-Type'],'application/octet-stream'); self.assertEqual(b,self.payload)
        self.assertEqual(self.request('/empty')[2],b'')
    def test_mime(self):
        mapping={'html':'text/html; charset=utf-8','htm':'text/html; charset=utf-8','CSS':'text/css; charset=utf-8','js':'text/javascript; charset=utf-8','txt':'text/plain; charset=utf-8','png':'image/png','jpg':'image/jpeg','jpeg':'image/jpeg','gif':'image/gif','svg':'image/svg+xml','ico':'image/vnd.microsoft.icon'}
        for ext,mime in mapping.items():
            c,h,b=self.request('/sample.'+ext); self.assertEqual(c,200); self.assertEqual(h['Content-Type'],mime); self.assertEqual(b,b'abc\x00def')
    def test_decode_once_and_query(self):
        self.assertEqual(self.request('/hello%20world.txt?q=ignored')[2],b'space')
        self.assertEqual(self.request('/hello%2520world.txt')[2],b'literal percent')
        self.assertEqual(self.request('/a+b.txt')[2],b'plus')
    def test_not_found(self):
        for path in ('/missing','/emptydir/','/index.html/','/index.html/child','/absent/../index.html'):
            self.assertEqual(self.request(path)[0],404,path)
    def test_traversal(self):
        for path in ('/../secret','/%2e%2e/secret','/docs/../../secret','/%2e%2e%2fsecret'):
            self.assertEqual(self.request(path)[0],403,path)
        self.assertEqual(self.request('/docs/../index.html')[2],b'hello world')
    def test_symlinks(self):
        for path in ('/link','/inside','/dirlink/index.html','/dirlink/../index.html','/linkedindex/'):
            self.assertEqual(self.request(path)[0],403,path)
    def test_invalid_encoding(self):
        for path in ('/%','/%2','/%GG','/%00','/%0d%0a'):
            self.assertEqual(self.request(path)[0],400,path)
    def test_redirect_backslash(self):
        (self.root/'\\docs').mkdir(exist_ok=True)
        self.assertEqual(self.request('/\\docs')[0],400)
    def test_nonregular_and_permissions(self):
        for path in ('/fifo','/socket','/denied','/blocked/'):
            self.assertEqual(self.request(path)[0],403,path)
    def test_method_policy(self):
        self.assertEqual(self.request('/', 'POST')[0],204)
        self.assertEqual(self.request('/', 'PUT')[0],501)
    def test_cleanup_and_recovery(self):
        before=len(list(pathlib.Path(f'/proc/{self.proc.pid}/fd').iterdir()))
        for _ in range(50):
            self.assertEqual(self.request('/missing')[0],404)
            self.assertEqual(self.request('/docs/')[0],200)
        self.assertEqual(len(list(pathlib.Path(f'/proc/{self.proc.pid}/fd').iterdir())),before)
    def test_bad_startup(self):
        for root in (self.base/'absent',self.root/'index.html',self.root/'dirlink',self.root/'blocked'):
            result=subprocess.run([SERVER,'18090',str(root),'4','64','100','1048576','30','10','1'],capture_output=True,timeout=3)
            self.assertNotEqual(result.returncode,0)
        self.assertFalse((self.base/'absent').exists())

if __name__=='__main__': unittest.main(verbosity=2)
