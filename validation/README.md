# 최종 검증 하네스

Docker Linux 저장소 루트에서 실행한다. 외부 부하 도구가 설치되지 않은 환경에서 Python 3 표준 라이브러리를 사용한다.

```sh
python3 validation/test_harness.py
python3 validation/run.py --smoke
python3 validation/run.py --full
```

`--full`은 기존 기능 회귀 후 백엔드별 직접 용량을 분리 측정한다. 이어 정적/프록시/혼합 × 동시 1·10·50·100·200 × 3회, 각 예비 5초·측정 30초와 혼합 동시 50·600초를 실행한다. 정식 조건을 축소하는 시간/횟수 옵션은 제공하지 않는다. `--smoke` 결과는 별도 경로이며 정식 결과를 대체하지 않는다.

큐 64·연결 100·워커 4·본문 1MiB·수신 30초·idle 10초·최대 100회·프록시 5/10초를 조건 사이에 변경하지 않는다. 현재 root와 결과 로그는 Windows bind mount 위이며 그 I/O 비용도 포함한다. 발생기·샘플러·두 백엔드·서버가 같은 CPU 자원을 공유한다.

`validation-results/UTC실행시각/`의 `progress.json`과 `heartbeat.json`으로 상태를 확인한다. `summary.json`은 회차마다 갱신한다. 모든 응답의 MIME·단일 CL·프레이밍·본문 바이트를 확인하며 첫 20개 오류만 상세 샘플로 남기고 전체 오류 수는 계속 센다.

지연 원시 자료는 zlib 압축된 native-endian uint32 히스토그램이다. 버킷 너비는 100µs, 인덱스는 `ceil(latency_us/100)`, 마지막은 60초 이상을 포함한다. 해당 지연은 요청 시작부터 전체 검증 완료까지이며 접속 시간과 bounded drain을 포함한다. p99는 순위가 들어가는 버킷 구간으로 표시하고 회차 p99를 평균하지 않는다.

정식 30초 안에 검증 완료한 요청으로 RPS를 계산한다. 30초 종료 당시 미완료는 `issued - completed_within_window`이며 15초 요청 기한까지 drain한 결과와 따로 기록한다. 예비 중 시작한 요청은 정식 초반에 완료돼도 예비 집단에 속한다. 예비 통계의 rate는 정식 성능 수치로 사용하지 않는다.

성능 측정이 모두 끝나고 서버·백엔드가 종료된 뒤 별도 안전성 검사를 실행한다.

```sh
python3 validation/safety.py --output validation-results/실행시각/safety
python3 validation/report.py validation-results/실행시각
```

ASan·UBSan·누수 검사는 일반 성능과 분리한다. TSan 시작 실패는 미완료이며 다른 검사의 통과로 대체하지 않는다. 접속 timeout의 실제 SYN DROP 증거는 별도 `validation-results/syn-drop/evidence.json`에 보관한다. 기존 단위 시계 대체 검사를 실제 네트워크 장애라고 보고하지 않는다.

최종 보고서·CSV·자원 시계열·분포·오류 로그를 함께 검토한다. 정상 부하의 예상 밖 실패는 한 건이라도 실패다. 과부하에서 설명되지 않는 deadline/도구 오류를 정책상 정상 결과로 간주하지 않는다.

`run.py`의 정상 종료는 조건 실행이 끝났다는 뜻이며 전체 품질 합격을 의미하지 않는다. `report.py`는 원시 결과로 판정하고 PASS는 0, FAIL은 1, INCOMPLETE는 2로 종료한다. 실패와 TSan 미완료가 함께 있으면 FAIL로 종료하되 각 항목은 따로 보관한다.
