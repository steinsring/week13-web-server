"""회차별 원시 결과를 합격/실패/미완료와 함께 요약한다. p99를 평균하지 않는다."""
import argparse
import collections
import csv
import json
from pathlib import Path
import statistics
import array
import zlib


def resource_metrics(directory, run):
    samples=[json.loads(line) for line in (directory/'raw'/(run['id']+'.resources.jsonl')).read_text().splitlines()]
    result={}
    for role in ['server','backend1','backend2','generator']:
        values=[(sample['elapsed'],sample['processes'][role]) for sample in samples if sample['phase']=='formal' and 'measurement_error' not in sample['processes'][role]]
        cpu=[]
        for (previous_time,previous),(now,current) in zip(values,values[1:]):
            cpu.append(100*(current['cpu_seconds']-previous['cpu_seconds'])/(now-previous_time))
        result[role]={'sampled_cpu_percent_one_core':statistics.mean(cpu) if cpu else None,
            'rss_kib_max':max((v['rss_kib'] for _,v in values),default=None),
            'fd_max':max((v['fds'] for _,v in values),default=None),
            'threads_max':max((v['threads'] for _,v in values),default=None)}
    result['listen_pending_max']=max((s['tcp']['listen_pending'] for s in samples),default=0)
    result['established_max']=max((s['tcp']['established'] for s in samples),default=0)
    return result


def main(directory):
    directory=Path(directory);runs=json.loads((directory/'summary.json').read_text())
    rows=[];groups=collections.defaultdict(list)
    for run in runs:
        metric=run['measurement'];resources=resource_metrics(directory,run)
        overload=run['concurrency']==200 and not run['id'].startswith('capacity')
        unexpected={key:value for key,value in metric['errors'].items() if not (overload and key=='http_503')}
        verdict='FAIL' if unexpected or (not overload and metric['errors']) or metric['unfinished_after_drain'] else 'PASS'
        row={'id':run['id'],'scenario':run['scenario'],'concurrency':run['concurrency'],
             'rps':metric['validated_success_rps'],'p50_ms_interval':json.dumps(metric['latency_ms']['p50']),
             'p95_ms_interval':json.dumps(metric['latency_ms']['p95']),'p99_ms_interval':json.dumps(metric['latency_ms']['p99']),
             'issued':metric['issued'],'incomplete_at_cutoff':metric['incomplete_at_window_end'],
             'errors':json.dumps(metric['errors']),'verdict':verdict,
             'warmup_errors':json.dumps(run['warmup']['errors']),
             'success_p99_ms_interval':json.dumps(metric['success_latency_ms']['p99']),
             'reuse_rate':metric['successful_reuse_rate'], 'resources':resources}
        rows.append(row)
        if not run['id'].startswith('capacity') and not run['id'].startswith('soak'):
            groups[(run['scenario'],run['concurrency'])].append(row)
    (directory/'derived-metrics.json').write_text(json.dumps(rows,indent=2))
    with (directory/'summary.csv').open('w',newline='',encoding='utf-8') as log:
        writer=csv.DictWriter(log,fieldnames=[k for k in rows[0] if k!='resources'])
        writer.writeheader();writer.writerows({k:v for k,v in row.items() if k!='resources'} for row in rows)
    normal_fail=[row['id'] for row in rows if row['concurrency']<200 and row['verdict']=='FAIL' and not row['id'].startswith('capacity')]
    overload_fail=[row['id'] for row in rows if row['concurrency']==200 and row['verdict']=='FAIL']
    safety_path=directory/'safety'/'summary.json'
    safety=json.loads(safety_path.read_text()) if safety_path.exists() else []
    tsan=next((value.get('classification','FAIL') for value in safety if value['name']=='tsan-attempt'),'NOT_RUN')
    safety_fail=[value['name'] for value in safety if value['returncode'] and value.get('classification')!='INCOMPLETE_ENVIRONMENT']
    recovery_fail=[]
    for path in directory.glob('*.recovery.json'):
        m=json.loads(path.read_text())['measurement']
        if m['errors'] or not m['success_with_drain'] or m['unfinished_after_drain']:
            recovery_fail.append(path.name)
    matrix=[r for r in runs if not r['id'].startswith(('capacity','soak'))]
    conditions=collections.Counter((r['scenario'],r['concurrency']) for r in matrix)
    required={(s,c):3 for s in ['static','proxy','mixed'] for c in [1,10,50,100,200]}
    complete=(dict(conditions)==required and all(r['duration_seconds']==30 and r['warmup_seconds']==5 for r in matrix)
              and any(r['id']=='soak-mixed-c050' and r['duration_seconds']==600 and r['concurrency']==50 for r in runs))
    verdict={'formal_conditions_complete':complete,'normal_failures':normal_fail,'overload_failures':overload_fail,
             'recovery_failures':recovery_fail,'safety_failures':safety_fail,'tsan':tsan,
             'overall':'FAIL' if normal_fail or overload_fail or recovery_fail or safety_fail else ('INCOMPLETE' if not complete or tsan!='PASS' else 'PASS')}
    (directory/'verdict.json').write_text(json.dumps(verdict,indent=2))
    lines=['# 최종 품질 검증 결과','',f'결과 경로: `{directory.name}`',
           '',f'최종 판정: **{verdict["overall"]}**. 명세의 정식 조건 실행: {complete}.',
           f'정상/지속 부하 실패: {normal_fail or "없음"}. 과부하 실패/원인 확인 필요: {overload_fail or "없음"}.',
           f'회복 실패: {recovery_fail or "없음"}. 안전성 검사 실패: {safety_fail or "없음"}. TSan: {tsan}.',
           '미완료 또는 실패가 있으면 전체 검증 통과로 해석하지 않는다.',
           '', '| 시나리오 | 동시 | 회차별 검증 RPS | RPS 범위 / CV | 회차별 p99 구간 ms | 판정 |',
           '| --- | ---: | --- | --- | --- | --- |']
    for (scenario,concurrent),values in groups.items():
        rps=[v['rps'] for v in values];mean=statistics.mean(rps);cv=statistics.stdev(rps)/mean*100 if len(rps)>1 and mean else 0
        intervals=[json.loads(v['p99_ms_interval']) for v in values]
        p99=', '.join('N/A' if v is None else f'({v["lower_ms"]:.1f}, {v["upper_ms"]:.1f}]' for v in intervals)
        lines.append(f'| {scenario} | {concurrent} | '+', '.join(f'{v:.2f}' for v in rps)+f' | {min(rps):.2f}–{max(rps):.2f} / {cv:.1f}% | '+p99+' | '+','.join(v['verdict'] for v in values)+' |')
    lines += ['', '## 직접 백엔드 용량 (정식 프록시 성능과 분리)', '',
        '| 조건 | 검증 RPS | 해당 백엔드 평균 CPU% | 발생기 평균 CPU% | 오류 |',
        '| --- | ---: | ---: | ---: | --- |']
    for row in rows:
        if row['id'].startswith('capacity'):
            backend='backend1' if 'backend1' in row['id'] else 'backend2'
            b=row['resources'][backend]['sampled_cpu_percent_one_core'];g=row['resources']['generator']['sampled_cpu_percent_one_core']
            lines.append(f'| {row["id"]} | {row["rps"]:.2f} | {b:.1f} | {g:.1f} | {row["errors"]} |')
    lines += ['', '각 백엔드는 별도로 측정했다. 두 직접 RPS를 공유 CPU에서도 동시에 낼 수 있다고 단순 합산하지 않는다. 직접 값·역할별 CPU와 프록시 조건을 함께 보며, 발생기 자체의 직렬 처리/바이트 검증과 Windows I/O도 포함된 결과다.']
    direct_cpu=[row['resources']['generator']['sampled_cpu_percent_one_core'] for row in rows if row['id'].startswith('capacity') and row['concurrency']==100]
    if direct_cpu:
        lines += [f'직접 동시100에서 단일 발생기 CPU가 {min(direct_cpu):.1f}–{max(direct_cpu):.1f}%여서 발생기 포화 가능성이 높다. 직접 RPS도 백엔드 단독 최대 용량으로 주장하지 않는다.']
    lines += ['', '## 자원 요약 (CPU 100%=한 코어)', '',
        '| 회차 | 서버 평균 CPU% / 최대 RSS KiB / FD / 스레드 | 발생기 평균 CPU% | 백엔드1/2 평균 CPU% |',
        '| --- | --- | --- | --- |']
    for row in rows:
        r=row['resources'];s=r['server'];g=r['generator'];b1=r['backend1'];b2=r['backend2']
        cpu=lambda v: 'N/A' if v is None else f'{v:.1f}'
        lines.append(f'| {row["id"]} | {cpu(s["sampled_cpu_percent_one_core"])} / {s["rss_kib_max"]} / {s["fd_max"]} / {s["threads_max"]} | {cpu(g["sampled_cpu_percent_one_core"])} | {cpu(b1["sampled_cpu_percent_one_core"])} / {cpu(b2["sampled_cpu_percent_one_core"])} |')
    soak=next((r for r in runs if r['id']=='soak-mixed-c050'),None)
    if soak:
        samples=[json.loads(line) for line in (directory/'raw'/'soak-mixed-c050.resources.jsonl').read_text().splitlines()]
        active=[s for s in samples if s['phase']=='formal']
        first=[s['processes']['server']['rss_kib'] for s in active if s['elapsed']<35]
        last=[s['processes']['server']['rss_kib'] for s in active if s['elapsed']>575]
        lines += ['', '## 600초 지속 부하', '',
            f'- 검증 성공 {soak["measurement"]["success_with_drain"]}건, 오류 {soak["measurement"]["errors"]}.',
            f'- 서버 RSS 첫30초 중앙값 {statistics.median(first) if first else None}KiB, 마지막30초 중앙값 {statistics.median(last) if last else None}KiB.',
            f'- 전/후 자원: `{soak["before"]["server"]}` → `{soak["after"]["server"]}`.',
            '- RSS는 캐시·할당기 보유를 포함한다. RSS 증감만으로 누수 유무를 확정하지 않고 별도 LSAN 결과와 구분한다.']
    lines += ['', '## 503 원인 판정의 근거', '',
        '관측은 503 상태·발생 수·저장된 응답/지연 샘플이다. 큐64·워커4·연결100이 고정됐고 소스 스냅샷의 503 경로는 첫 작업 큐 포화 또는 작업 대기10초 초과다. 아래 오류 분포가10초보다 짧으면 큐 포화와 일치하지만, 별도 내부 카운터를 주입한 확정 경로 추적은 아니다.']
    for run in runs:
        if set(run['measurement']['errors'])=={'http_503'}:
            all_hist=array.array('I');all_hist.frombytes(zlib.decompress((directory/'raw'/(run['id']+'.all.hist.z')).read_bytes()))
            success_hist=array.array('I');success_hist.frombytes(zlib.decompress((directory/'raw'/(run['id']+'.success.hist.z')).read_bytes()))
            indices=[i for i,(a,s) in enumerate(zip(all_hist,success_hist)) if a>s]
            lines.append(f'- {run["id"]}: 503 {run["measurement"]["errors"]["http_503"]}건, 관측 오류 지연 상한 {max(indices)*.1:.1f}ms, 성공 본문 훼손 0건 여부는 원본 error 목록 참조.')
    lines += ['', '## 집계 경계', '',
        '- 각 회차는 예비 5초 뒤 정식 30초다. 동시 200도 세 시나리오별 3회이며 정상 표와 구분한다. 지속 부하는 혼합 동시 50·600초다.',
        '- RPS는 정식 시간 안에 완료·본문 검증한 요청만 센다. 종료 시 미완료와 15초 기한 이내 drain 결과를 별도 보관한다.',
        '- 지연은 정식 시간 안에 시작한 요청 전체가 대상이며 drain 완료도 포함한다. 100µs ceil 히스토그램의 분위수 구간을 표시하며 p99를 평균하지 않는다.',
        '- 예비 시간에 시작해 정식 초반까지 진행한 요청은 예비 통계로 분류한다. 그 요청이 정식 초반의 outstanding 슬롯을 점유할 수 있다.',
        '- 예비 통계의 rate 필드는 교차 완료까지 포함하므로 따로 측정한 예비 처리량으로 사용하지 않는다. 정식 RPS와 정식 cutoff 집계에는 영향을 주지 않는다.',
        '- 모든 정상 200의 MIME·단일 CL·TE 부재·길이·본문 바이트를 확인한다. 예상 밖 HTTP 오류·deadline·본문 훼손을 정상 부하의 실패로 센다.',
        '- 정식 행렬의 오류 응답은 길이와 상태까지 읽었다. 503 상태명 본문 의미는 이후 같은 동시200의 별도 ASan 기능 부하에서 보완하며 그 검증을 정식 RPS에 섞지 않는다.',
        '- 과부하에서 503만 기존 큐 포화/대기 기한 정책 결과로 분류한다. 생성기 deadline이나 설명되지 않는 접속 오류는 자동 합격시키지 않는다.',
        '- 오류가 생긴 혼합 조건은 성공한 요청 유형의 비율도 달라진다. 과부하의 검증 성공 RPS가 높다고 동일한 유효 워크로드 용량이 향상됐다고 해석하지 않는다.',
        '', '## 자원과 환경 한계', '',
        '- 서버와 두 Python 백엔드, 단일 asyncio 발생기가 같은 Docker/WSL2 CPU·파일시스템을 공유한다. 서버 단독의 최대 용량이 아닌 본문 검증을 포함한 end-to-end 처리량이다.',
        '- fixture 루트와 원시 결과는 Windows bind mount 위에 있다. 파일 I/O·계측 로그 쓰기 비용이 포함되며 따뜻한 캐시 조건이다.',
        '- 1초 /proc 계측 비용은 회차 sampling.spent_ms에 기록하며 발생기 CPU에 포함한다. 실제 시간 간격과 각 역할의 CPU 누적·RSS·FD·스레드는 원시 시계열에 있다.',
        '- 직접 백엔드 용량은 각각 동시 1/100, 예비 5초·측정 10초이고 upstream처럼 요청당 새 연결이다. 프런트 측정은 keep-alive이므로 그 차이도 포함해 해석한다.',
        '- 10분과 동시 200으로 24시간 안정성·C10K·모든 트래픽 성능을 보장하지 않는다.',
        '', '## 증거', '',
        '- environment.json / commands.json / source-manifest.json / source/: 버전·자원·실행 인자·실제 소스 스냅샷',
        '- raw/*.json / *.resources.jsonl / *.hist.z: 회차 지표·자원·압축된 100µs 지연 분포',
        '- *.recovery.json: 각 회차 뒤 정확한 저부하 응답과 자원 회복',
        '- regression/ 및 safety/: 기능 회귀·sanitizer·TSan 원본 명령과 로그',
        '- TSan은 순차 GET/POST 사례 하나가 통과했으나 반복 자원 사례가 unexpected memory mapping으로 시작하지 못했다. 전체 경합 검증은 미완료다.',
        '- ../syn-drop/evidence.json: 격리 Docker에서 실제 SYN DROP·504·복구 증거(별도 부모 실행)',
        '- 측정 실패·미완료를 숨기거나 서버의 큐/연결/시간 상한을 높여 통과시키지 않았다.']
    (directory/'REPORT.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print(directory/'REPORT.md')
    return 1 if verdict['overall']=='FAIL' else (2 if verdict['overall']=='INCOMPLETE' else 0)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('directory');raise SystemExit(main(parser.parse_args().directory))
