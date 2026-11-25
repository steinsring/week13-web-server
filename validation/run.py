"""최종 명세의 고정 행렬을 실행한다. --smoke는 별도 결과이며 정식 측정을 대체하지 않는다."""
import argparse
import asyncio
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import subprocess
import sys
import time
from fixtures import prepare
from load import run_load, process_sample

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ['proxy.c', 'proxy_config.c', 'http_body.c', 'concurrency.c', 'static_files.c',
           'server.c', 'http_request.c', 'http_response.c', 'request_io.c']


def command(args):
    result = subprocess.run(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return {'command': args, 'returncode': result.returncode, 'output': result.stdout}


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def environment():
    paths = ['/sys/fs/cgroup/cpu.max', '/sys/fs/cgroup/memory.max', '/sys/fs/cgroup/cpuset.cpus.effective',
             '/proc/meminfo', '/proc/cpuinfo', '/proc/sys/net/core/somaxconn',
             '/proc/sys/net/ipv4/tcp_tw_reuse', '/proc/sys/net/ipv4/ip_local_port_range']
    return {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'kernel': platform.platform(), 'python': sys.version, 'cpu_count': os.cpu_count(),
            'tool_paths': {name: shutil.which(name) for name in ['wrk', 'ab', 'hey', 'gcc', 'python3']},
            'resource_files': {path: Path(path).read_text() for path in paths},
            'git_head': command(['git', 'rev-parse', 'HEAD']), 'git_status': command(['git', 'status', '--short']),
            'compiler': command(['gcc', '--version']),
            'generator': 'Python asyncio standard library; response bytes verified; same two-CPU Docker host',
            'cache': 'warm OS file cache after explicit 5-second warmup; not cold cache',
            'latency': 'monotonic request start incl connect to fully verified response; 15s request deadline',
            'percentiles': 'per-run rank from bounded 100us ceil histogram; intervals, never averaged p99',
            'sampling': '1-second /proc sampler in generator; sampled CPU totals include sampler overhead'}


async def main(smoke):
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    result_dir = ROOT/'validation-results'/(("smoke-" if smoke else "")+timestamp)
    result_dir.mkdir(parents=True)
    logs = result_dir/'raw'; logs.mkdir()
    manifest = prepare(result_dir/'fixtures')
    (result_dir/'environment.json').write_text(json.dumps(environment(), indent=2))
    (result_dir/'source.diff').write_text(command(['git', 'diff'])['output'])
    snapshot = result_dir/'source'
    snapshot.mkdir()
    sources = [*ROOT.glob('*.c'), *ROOT.glob('*.h'), ROOT/'Makefile', ROOT/'Dockerfile', ROOT/'start.sh',
               *ROOT.glob('validation/*.py')]
    source_hashes = {}
    for path in sources:
        relative = path.relative_to(ROOT)
        destination = snapshot/relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(path.read_bytes())
        source_hashes[str(relative)] = hashlib.sha256(path.read_bytes()).hexdigest()
    (result_dir/'source-manifest.json').write_text(json.dumps(source_hashes, indent=2))
    binary = Path('/tmp')/f'ljm-validation-{timestamp}'
    build = command(['gcc', '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-O2', '-pthread',
                     '-I.', *SOURCES, '-o', str(binary)])
    (result_dir/'build.json').write_text(json.dumps(build, indent=2))
    if build['returncode']:
        raise RuntimeError(build['output'])
    processes = []
    open_logs = []
    runs = []
    backend_ports = [free_port(), free_port()]
    server_port = free_port()

    def start(args, role):
        error = (result_dir/f'{role}.stderr.log').open('w')
        open_logs.append(error)
        proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=error)
        processes.append(proc)
        if not proc.stdout.readline().startswith(b'Listening'):
            raise RuntimeError(f'{role} failed; see {role}.stderr.log')
        return proc

    def progress(state, **extra):
        value = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                 'state': state, 'completed_runs': len(runs), **extra}
        (result_dir/'progress.json').write_text(json.dumps(value, indent=2))
        print(json.dumps(value), flush=True)

    async def measure(run_id, scenario, concurrency, port, roles, duration=30, warmup=5, direct=False):
        progress('RUNNING', run=run_id, seconds=duration, warmup=warmup)
        result = await run_load(logs, run_id, scenario, concurrency, port, roles, duration, warmup, direct)
        runs.append(result)
        (result_dir/'summary.json').write_text(json.dumps(runs, indent=2))
        progress('RUN_COMPLETED', run=run_id, rps=result['measurement']['validated_success_rps'],
                 errors=result['measurement']['errors'])
        return result

    try:
        if not smoke:
            progress('FUNCTIONAL_PREFLIGHT')
            preflight = command([sys.executable, 'validation/regression.py', '--output', str(result_dir/'regression')])
            (result_dir/'functional-preflight.json').write_text(json.dumps(preflight, indent=2))
            if preflight['returncode']:
                raise RuntimeError('Functional preflight failed; see regression/summary.json')
        backends = [start([sys.executable, 'validation/backend.py', str(port), str(result_dir/f'backend-{i+1}.jsonl')],
                          f'backend-{i+1}') for i, port in enumerate(backend_ports)]
        argv = [str(binary), str(server_port), str(result_dir/'fixtures'), '4', '128', '100', '1048576', '30',
                '10', '100', f'http://localhost:{server_port}', '5', '10',
                ','.join(f'http://127.0.0.1:{port}' for port in backend_ports)]
        server = start(argv, 'server')
        roles = {'server': server.pid, 'backend1': backends[0].pid, 'backend2': backends[1].pid, 'generator': os.getpid()}
        (result_dir/'commands.json').write_text(json.dumps({'server': argv, 'roles': roles, 'fixtures': manifest,
            'runner': [sys.executable, 'validation/run.py', '--smoke' if smoke else '--full'],
            'configuration_fixed_for_all_conditions': True}, indent=2))

        # 재현 가능한 직접 백엔드 용량 표는 공식 프록시 행렬과 분리한다.
        for i, port in enumerate(backend_ports):
            for concurrency in ([1] if smoke else [1, 100]):
                await measure(f'capacity-backend{i+1}-c{concurrency}', 'proxy', concurrency, port, roles,
                              duration=2 if smoke else 10, warmup=1 if smoke else 5, direct=True)
        progress('CAPACITY_COMPLETE_FORMAL_MATRIX_START')
        for scenario in ['static', 'proxy', 'mixed']:
            for concurrency in ([1, 10] if smoke else [1, 10, 50, 100, 200]):
                for repeat in range(1, 2 if smoke else 4):
                    run_id = f'{scenario}-c{concurrency:03d}-r{repeat}'
                    await measure(run_id, scenario, concurrency, server_port, roles,
                                  duration=2 if smoke else 30, warmup=1 if smoke else 5)
                    # 회차 끝 연결을 닫은 뒤 정확성·FD 회수를 새 저부하로 확인한다.
                    await asyncio.sleep(.2)
                    recovery = await run_load(logs, run_id+'-recovery', 'mixed', 1, server_port, roles,
                                              duration=.5, warmup=.1)
                    (result_dir/f'{run_id}.recovery.json').write_text(json.dumps(recovery, indent=2))
                    if server.poll() is not None:
                        raise RuntimeError(f'server crashed in {run_id}')
        await measure('soak-mixed-c050', 'mixed', 50, server_port, roles,
                      duration=2 if smoke else 600, warmup=1 if smoke else 5)
        progress('MEASUREMENT_COMPLETE', result_directory=str(result_dir))
    except BaseException as error:
        progress('INTERRUPTED', error=repr(error))
        raise
    finally:
        exits = {}
        for proc in reversed(processes):
            if proc.poll() is None:
                proc.terminate()
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait(); exits[str(proc.pid)] = 'forced by harness after 15s'
            exits[str(proc.pid)] = proc.returncode
        for log in open_logs:
            log.close()
        (result_dir/'exit-codes.json').write_text(json.dumps(exits, indent=2))
        try:
            binary.unlink()
        except FileNotFoundError:
            pass
    print('RESULT_DIRECTORY='+str(result_dir), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--full', action='store_true')
    mode.add_argument('--smoke', action='store_true')
    args = parser.parse_args()
    asyncio.run(main(args.smoke))
