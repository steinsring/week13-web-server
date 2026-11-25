"""표준 라이브러리로 만든 유한 HTTP 검증 발생기. 성능 결과에 계측 비용이 포함된다."""
import array
import asyncio
import collections
import json
import math
import os
from pathlib import Path
import time
import zlib
from fixtures import SMALL, LARGE, API_GET, API_POST

HISTOGRAM_US = 100
HISTOGRAM_BINS = 600001
DEADLINE_SECONDS = 15


class Stats:
    def __init__(self):
        self.issued = self.completed = self.window_completed = self.success = self.window_success = 0
        self.connections = self.reused = self.connect_attempts = 0
        self.reused_success = self.server_closes = 0
        self.workloads = collections.Counter()
        self.errors = collections.Counter()
        self.examples = []
        self.histogram = array.array('I', [0]) * HISTOGRAM_BINS
        self.success_histogram = array.array('I', [0]) * HISTOGRAM_BINS
        self.latency_max_us = 0

    def finish(self, latency, success, error, within_window, detail=''):
        self.completed += 1
        self.window_completed += within_window
        self.success += success
        self.window_success += success and within_window
        micros = math.ceil(latency * 1e6)
        self.latency_max_us = max(self.latency_max_us, micros)
        bucket = min(HISTOGRAM_BINS-1, math.ceil(micros / HISTOGRAM_US))
        self.histogram[bucket] += 1
        if success:
            self.success_histogram[bucket] += 1
        else:
            self.errors[error] += 1
            if len(self.examples) < 20:
                self.examples.append({'kind': error, 'latency_ms': latency*1000, 'detail': detail[:400]})

    def summary(self, duration):
        def quantile(hist, fraction):
            target = math.ceil(sum(hist)*fraction)
            if not target:
                return None
            count = 0
            for i, n in enumerate(hist):
                count += n
                if count >= target:
                    return {'lower_ms': max(0, i-1)*HISTOGRAM_US/1000,
                            'upper_ms': i*HISTOGRAM_US/1000}
        return {'issued': self.issued, 'completed_with_drain': self.completed,
                'completed_within_window': self.window_completed,
                'incomplete_at_window_end': self.issued-self.window_completed,
                'success_with_drain': self.success, 'success_within_window': self.window_success,
                'unfinished_after_drain': self.issued-self.completed,
                'completion_rps': self.window_completed/duration, 'validated_success_rps': self.window_success/duration,
                'errors': dict(self.errors), 'error_examples': self.examples,
                'new_connections': self.connections, 'connection_attempts': self.connect_attempts,
                'reused_requests': self.reused, 'reuse_rate': self.reused/self.issued if self.issued else 0,
                'successful_reused_requests': self.reused_success,
                'successful_reuse_rate': self.reused_success/self.success if self.success else 0,
                'response_connection_close': self.server_closes, 'workload_counts': dict(self.workloads),
                'latency_population': 'requests started inside formal window; completion includes bounded drain',
                'latency_quantization_us': HISTOGRAM_US,
                'latency_ms': {p: quantile(self.histogram, q) for p, q in [('p50', .5), ('p95', .95), ('p99', .99)]},
                'success_latency_ms': {p: quantile(self.success_histogram, q) for p, q in [('p50', .5), ('p95', .95), ('p99', .99)]},
                'latency_max_ms': self.latency_max_us/1000}


def process_sample(pid):
    try:
        directory = Path(f'/proc/{pid}')
        fields = (directory/'stat').read_text().rsplit(')', 1)[1].split()
        status = dict(line.split(':', 1) for line in (directory/'status').read_text().splitlines() if ':' in line)
        return {'pid': pid, 'cpu_seconds': (int(fields[11])+int(fields[12]))/os.sysconf('SC_CLK_TCK'),
                'rss_kib': int(status['VmRSS'].split()[0]), 'threads': int(status['Threads']),
                'fds': len(list((directory/'fd').iterdir()))}
    except (OSError, KeyError, ValueError) as error:
        return {'pid': pid, 'measurement_error': str(error)}


def tcp_sample(port):
    result = {'established': 0, 'syn_received': 0, 'listen_pending': 0}
    for name in ('tcp', 'tcp6'):
        for line in Path('/proc/net', name).read_text().splitlines()[1:]:
            values = line.split()
            if int(values[1].split(':')[1], 16) != port:
                continue
            state = values[3]
            result['established'] += state == '01'
            result['syn_received'] += state == '03'
            if state == '0A':
                result['listen_pending'] += int(values[4].split(':')[1], 16)
    return result


def workload(scenario, sequence):
    if scenario == 'static':
        choices = [('GET', '/small.txt', b'', SMALL), ('GET', '/large.bin', b'', LARGE)]
    elif scenario == 'proxy':
        choices = [('GET', '/api/get', b'', API_GET), ('POST', '/api/post', API_POST, API_POST)]
    else:
        choices = [('GET', '/small.txt', b'', SMALL), ('GET', '/api/get', b'', API_GET),
                   ('GET', '/large.bin', b'', LARGE), ('POST', '/api/post', API_POST, API_POST)]
    return choices[sequence % len(choices)]


class Client:
    def __init__(self, port, direct):
        self.port, self.direct = port, direct
        self.reader = self.writer = None
        self.request_count = 0
        self.stage = 'none'

    async def close(self):
        if self.writer:
            self.writer.close()
            try:
                await asyncio.wait_for(self.writer.wait_closed(), 1)
            except (ConnectionError, asyncio.TimeoutError):
                pass
        self.reader = self.writer = None

    async def execute(self, choice, stats):
        method, path, body, expected = choice
        reused = self.writer is not None
        self.stage = 'connect'
        if self.writer is None:
            stats.connect_attempts += 1
            self.reader, self.writer = await asyncio.open_connection('127.0.0.1', self.port, limit=16384)
            stats.connections += 1
            self.request_count = 0
        else:
            stats.reused += 1
        self.stage = 'send'
        self.writer.write(f'{method} {path} HTTP/1.1\r\nHost: validation\r\nContent-Length: {len(body)}\r\n'.encode()
                          + (b'Connection: close\r\n' if self.direct else b'') + b'\r\n' + body)
        await self.writer.drain()
        self.stage = 'headers'
        header = await self.reader.readuntil(b'\r\n\r\n')
        lines = header[:-4].split(b'\r\n')
        status = int(lines[0].split()[1])
        pairs = [line.split(b':', 1) for line in lines[1:]]
        headers = {name.lower(): value.strip().lower() for name, value in pairs}
        self.stage = 'body'
        declared = int(headers[b'content-length'])
        expected_type = b'text/plain' if path == '/small.txt' else b'application/octet-stream'
        matches = (declared == len(expected) and headers.get(b'content-type', b'').startswith(expected_type)
                   and sum(name.lower() == b'content-length' for name, _ in pairs) == 1
                   and b'transfer-encoding' not in headers)
        offset = 0
        remaining = declared
        # 출력 전체를 따로 적재하지 않는다. 모든 수신 바이트를 fixture와 비교한다.
        while remaining:
            chunk = await self.reader.readexactly(min(65536, remaining))
            if chunk != expected[offset:offset+len(chunk)]:
                matches = False
            offset += len(chunk)
            remaining -= len(chunk)
        self.request_count += 1
        if headers.get(b'connection') == b'close':
            stats.server_closes += 1
            await self.close()
        if status != 200:
            return False, f'http_{status}', header[:250].decode(errors='replace')
        if not matches:
            return False, 'body_mismatch', f'declared={declared},expected={len(expected)}'
        stats.reused_success += reused
        return True, '', ''


async def run_load(directory, run_id, scenario, concurrent, port, roles, duration=30, warmup=5, direct=False):
    directory = Path(directory)
    warm, formal = Stats(), Stats()
    start = time.monotonic()
    formal_start, end = start+warmup, start+warmup+duration
    sequence = 0
    sampler_stop = asyncio.Event()
    sampling_ns = 0
    samples = 0
    before = {role: process_sample(pid) for role, pid in roles.items()}

    async def sampler():
        nonlocal sampling_ns, samples
        with (directory/f'{run_id}.resources.jsonl').open('w', buffering=1) as log:
            while not sampler_stop.is_set():
                tick = time.perf_counter_ns()
                value = {'run': run_id, 'utc_ns': time.time_ns(), 'elapsed': time.monotonic()-start,
                    'phase': 'warmup' if time.monotonic()<formal_start else ('formal' if time.monotonic()<end else 'drain'),
                    'processes': {role: process_sample(pid) for role, pid in roles.items()},
                    'tcp': tcp_sample(port), 'issued': formal.issued, 'completed': formal.completed,
                    'success': formal.success, 'errors': dict(formal.errors)}
                encoded = json.dumps(value)
                log.write(encoded+'\n')
                (directory.parent/'heartbeat.json').write_text(encoded)
                sampling_ns += time.perf_counter_ns()-tick
                samples += 1
                try:
                    await asyncio.wait_for(sampler_stop.wait(), timeout=1)
                except asyncio.TimeoutError:
                    pass

    async def worker():
        nonlocal sequence
        client = Client(port, direct)
        try:
            while time.monotonic() < end:
                now = time.monotonic()
                stats = warm if now < formal_start else formal
                choice = workload(scenario, sequence)
                sequence += 1
                stats.issued += 1
                stats.workloads[choice[0]+' '+choice[1]] += 1
                began = time.monotonic()
                try:
                    success, error, detail = await asyncio.wait_for(client.execute(choice, stats), DEADLINE_SECONDS)
                except asyncio.TimeoutError:
                    success, error, detail = False, 'deadline_'+client.stage, '15s request deadline'
                    await client.close()
                except (OSError, EOFError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, KeyError, ValueError) as exc:
                    success, error, detail = False, type(exc).__name__+'_'+client.stage, str(exc)
                    await client.close()
                finished = time.monotonic()
                stats.finish(finished-began, success, error, finished <= end, detail)
        finally:
            await client.close()

    sample_task = asyncio.create_task(sampler())
    await asyncio.gather(*(worker() for _ in range(concurrent)))
    sampler_stop.set()
    await sample_task
    elapsed = time.monotonic()-start
    after = {role: process_sample(pid) for role, pid in roles.items()}
    result = {'id': run_id, 'scenario': scenario, 'concurrency': concurrent,
              'duration_seconds': duration, 'warmup_seconds': warmup, 'actual_total_with_drain': elapsed,
              'cutoff_utc_ns': time.time_ns()-int((time.monotonic()-end)*1e9),
              'histogram_format': 'little/native-endian uint32 counts; zlib; 100us ceil buckets; last bin >=60s',
              'direct_backend_connection_close': direct, 'measurement': formal.summary(duration),
              'warmup': warm.summary(warmup), 'before': before, 'after': after,
              'sampling': {'period_seconds': 1, 'samples': samples, 'spent_ms': sampling_ns/1e6,
                           'generator_cpu_includes_sampler': True}}
    for suffix, hist in [('all', formal.histogram), ('success', formal.success_histogram)]:
        (directory/f'{run_id}.{suffix}.hist.z').write_bytes(zlib.compress(hist.tobytes()))
    (directory/f'{run_id}.json').write_text(json.dumps(result, indent=2))
    return result
