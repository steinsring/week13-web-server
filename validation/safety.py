"""성능 수치와 분리한 sanitizer·과부하·단절·종료 검사 및 TSan 재시도."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from fixtures import prepare
from load import run_load
from run import ROOT, SOURCES, free_port


async def main(output):
    output=Path(output);output.mkdir(parents=True,exist_ok=True)
    results=[]
    def run(name,args,env=None):
        began=time.time()
        with (output/(name+'.log')).open('w') as log:
            result=subprocess.run(args,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,env=env)
        value={'name':name,'command':args,'returncode':result.returncode,'elapsed_seconds':time.time()-began}
        results.append(value);(output/'summary.json').write_text(json.dumps(results,indent=2))
        print(name+': '+str(result.returncode),flush=True)
        return value
    flags=['gcc','-std=c11','-Wall','-Wextra','-Wpedantic','-O1','-g','-pthread','-fno-omit-frame-pointer','-I.']
    asan='/tmp/validation-server-asan'
    build=run('build-asan',flags+['-fsanitize=address,undefined',*SOURCES,'-o',asan])
    if build['returncode']:return 1
    run('unit-sanitizers',['make','sanitize'])
    env=dict(os.environ,TEST_SERVER=asan,ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    for test in ['proxy','keep_alive','post_body']:
        run('asan-'+test,[sys.executable,f'tests/test_{test}.py'],env)
    delay='/tmp/validation-server-delay-asan'
    if not run('build-delay-asan',flags+['-fsanitize=address,undefined',*SOURCES,'tests/slow_file.c','-Wl,--wrap=openat','-o',delay])['returncode']:
        run('asan-concurrency',[sys.executable,'tests/test_concurrency.py'],dict(env,TEST_SERVER=delay))

    with tempfile.TemporaryDirectory(prefix='ljm-safety-') as fixture:
        prepare(fixture);processes=[];ports=[free_port(),free_port()]
        logs=[]
        stresses=[];recovery=None
        try:
            for i,port in enumerate(ports):
                log=(output/f'stress-backend{i+1}.stderr.log').open('w');logs.append(log)
                p=subprocess.Popen([sys.executable,'validation/backend.py',str(port),str(output/f'stress-backend{i+1}.jsonl')],cwd=ROOT,stdout=subprocess.PIPE,stderr=log)
                processes.append(p);assert p.stdout.readline().startswith(b'Listening')
            port=free_port();error=(output/'stress-server.stderr.log').open('w');logs.append(error)
            server=subprocess.Popen([asan,str(port),fixture,'4','64','100','1048576','30','10','100',f'http://localhost:{port}','5','10',','.join(f'http://127.0.0.1:{p}' for p in ports)],cwd=ROOT,stdout=subprocess.PIPE,stderr=error,env=env)
            processes.append(server);assert server.stdout.readline().startswith(b'Listening')
            roles={'server':server.pid,'backend1':processes[0].pid,'backend2':processes[1].pid,'generator':os.getpid()}
            for scenario in ['static','proxy','mixed']:
                stresses.append(await run_load(output,'asan-overload-'+scenario+'-c200',scenario,200,port,roles,duration=5,warmup=1))
            recovery=await run_load(output,'asan-recovery-c1','mixed',1,port,roles,duration=2,warmup=1)
        finally:
            exit_codes=[]
            for p in reversed(processes):
                if p.poll() is None:p.terminate()
                p.wait(timeout=15);exit_codes.append(p.returncode)
            for log in logs:log.close()
            (output/'stress-exits.json').write_text(json.dumps(exit_codes))
            content_errors={stress['id']:{key:value for key,value in stress['measurement']['errors'].items() if key!='http_503'}
                            for stress in stresses if any(key!='http_503' for key in stress['measurement']['errors'])}
            recovery_metric=recovery['measurement'] if recovery else {'errors':{'setup_incomplete':1},'success_with_drain':0}
            server_log=output/'stress-server.stderr.log'
            diagnostics=server_log.read_text() if server_log.exists() else 'server setup incomplete'
            ok=(not any(exit_codes) and not diagnostics and len(stresses)==3
                and not content_errors and not recovery_metric['errors']
                and recovery_metric['success_with_drain']>0)
            results.append({'name':'asan-overload-and-recovery','returncode':0 if ok else 1,
                            'unexplained_stress_errors':content_errors,
                            'recovery_errors':recovery_metric['errors']})
    tsan='/tmp/validation-server-tsan'
    if not run('build-tsan',flags+['-fsanitize=thread',*SOURCES,'-o',tsan])['returncode']:
        value=run('tsan-attempt',[sys.executable,'tests/test_keep_alive.py','KeepAliveTests.test_sequential_get_post_and_empty','KeepAliveTests.test_repeated_request_resources'],dict(os.environ,TEST_SERVER=tsan,TSAN_OPTIONS='halt_on_error=1'))
        evidence=(output/'tsan-attempt.log').read_text()
        value['classification']='INCOMPLETE_ENVIRONMENT' if 'unexpected memory mapping' in evidence else ('PASS' if value['returncode']==0 else 'FAIL')
    (output/'summary.json').write_text(json.dumps(results,indent=2))
    return 1 if any(r['returncode'] and r.get('classification')!='INCOMPLETE_ENVIRONMENT' for r in results) else 0


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--output',required=True)
    raise SystemExit(asyncio.run(main(parser.parse_args().output)))
