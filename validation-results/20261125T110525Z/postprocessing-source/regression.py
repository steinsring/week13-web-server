"""성능 측정 전 기능 회귀를 실제 실행하고 명령·stdout/stderr를 보관한다."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from run import ROOT, SOURCES


def main(output):
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    results = []

    def run(name, args, env=None, user=None):
        began=time.time()
        with (output/(name+'.log')).open('w') as log:
            result=subprocess.run(args,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,
                                  env=env, user=user, group=user, extra_groups=[] if user else None)
        results.append({'name':name,'command':args,'returncode':result.returncode,'elapsed_seconds':time.time()-began})
        (output/'summary.json').write_text(json.dumps(results,indent=2))
        print(f'{name}: {result.returncode}',flush=True)
        return result.returncode

    run('harness',['python3','validation/test_harness.py'])
    run('unit',['make','all','test'])
    for name in ['proxy','keep_alive','post_body']:
        run(name,['python3',f'tests/test_{name}.py'])
    run('static',['python3','tests/test_static_files.py'],user=65534)
    binary='/tmp/validation-concurrency'
    if not run('build-concurrency',['gcc','-std=c11','-O2','-pthread','-I.',*SOURCES,'tests/slow_file.c','-Wl,--wrap=openat','-o',binary]):
        run('concurrency',['python3','tests/test_concurrency.py'],env=dict(os.environ,TEST_SERVER=binary))
    fail='/tmp/validation-alloc-fail'
    if not run('build-alloc',['gcc','-std=c11','-O2','-pthread','-Drealloc=test_realloc','-I.',*SOURCES,'tests/fail_alloc.c','-o',fail]):
        with tempfile.TemporaryDirectory() as root:
            Path(root,'index.html').write_bytes(b'hello world');processes=[];ports=[]
            try:
                for executable in [str(ROOT/'server'),fail]:
                    with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=str(probe.getsockname()[1]);ports.append(port)
                    args=[executable,port,root,'2','16','100','1048576','30','10','1','http://localhost:8080','5','10','http://127.0.0.1:9']
                    proc=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE);processes.append(proc)
                    assert proc.stdout.readline().startswith(b'Listening')
                run('errors',['python3','tests/test_server.py'],env=dict(os.environ,SERVER_TEST_PORT=ports[0],SERVER_ALLOC_FAIL_PORT=ports[1]))
            finally:
                for proc in processes:proc.terminate();proc.communicate(timeout=13)
    return 1 if any(result['returncode'] for result in results) else 0


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',required=True)
    raise SystemExit(main(parser.parse_args().output))
