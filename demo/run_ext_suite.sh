#!/usr/bin/env sh
set -eu

MODE="${1:-all}"

run_fuzz() {
    echo "=========================================="
    echo "[1/4] libFuzzer (ASan + UBSan) - 3 Harnesses"
    echo "=========================================="
    mkdir -p /tmp/fuzz-bin /tmp/fuzz-corpus/req /tmp/fuzz-corpus/static /tmp/fuzz-corpus/proxy

    # Seed corpora so libFuzzer immediately explores deep protocol branches
    printf '0GET /api/test?a=1 HTTP/1.1\r\nHost: localhost:8080\r\nContent-Length: 5\r\n\r\nhello' > /tmp/fuzz-corpus/req/seed1
    printf '1POST /upload HTTP/1.1\r\nHost: [::1]:8080\r\nExpect: 100-continue\r\nContent-Length: 10\r\n\r\n' > /tmp/fuzz-corpus/req/seed2
    printf '/index.html' > /tmp/fuzz-corpus/static/seed1
    printf '/docs' > /tmp/fuzz-corpus/static/seed2
    printf '/%%2e%%2e/etc/passwd' > /tmp/fuzz-corpus/static/seed3
    printf '/symlink.txt' > /tmp/fuzz-corpus/static/seed4
    printf '8HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\ntest\r\n0\r\n\r\n' > /tmp/fuzz-corpus/proxy/seed1
    printf '4HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:8001/api/next\r\nContent-Length: 0\r\n\r\n' > /tmp/fuzz-corpus/proxy/seed2

    clang -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -I. \
        tests/fuzz/fuzz_http_request.c http_request.c http_body.c proxy_config.c \
        -o /tmp/fuzz-bin/fuzz_http_request

    clang -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -I. \
        tests/fuzz/fuzz_static_files.c static_files.c http_response.c http_request.c \
        -o /tmp/fuzz-bin/fuzz_static_files

    clang -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -I. \
        tests/fuzz/fuzz_proxy_wire.c proxy_config.c http_request.c http_body.c \
        -o /tmp/fuzz-bin/fuzz_proxy_wire

    echo "--- Fuzzing #1: HTTP/1.1 Request Parser + Body + Routing (20s) ---"
    ASAN_OPTIONS=detect_leaks=1 /tmp/fuzz-bin/fuzz_http_request /tmp/fuzz-corpus/req \
        -max_total_time=20 -max_len=9000 -print_final_stats=1 2>&1 | tail -n 12

    echo "--- Fuzzing #2: Static Path Decoder + MIME + O_NOFOLLOW (20s) ---"
    ASAN_OPTIONS=detect_leaks=1 /tmp/fuzz-bin/fuzz_static_files /tmp/fuzz-corpus/static \
        -max_total_time=20 -max_len=1024 -print_final_stats=1 2>&1 | tail -n 12

    echo "--- Fuzzing #3: Reverse Proxy Upstream Wire Parser + Location (20s) ---"
    ASAN_OPTIONS=detect_leaks=1 /tmp/fuzz-bin/fuzz_proxy_wire /tmp/fuzz-corpus/proxy \
        -max_total_time=20 -max_len=8192 -print_final_stats=1 2>&1 | tail -n 12
}

start_stack() {
    PORT="${1:-8090}"
    CONN_LIMIT="${2:-250}"
    rm -rf /tmp/ext-public
    cp -r /app/public /tmp/ext-public
    ln -sf /etc/hostname /tmp/ext-public/symlink-blocked.txt
    head -c 4096 /dev/zero | tr '\0' 'A' > /tmp/ext-public/4kb.txt

    python3 /app/demo/demo_backend.py --name backend-1 --port 8101 >/tmp/b1.log 2>&1 &
    B1=$!
    python3 /app/demo/demo_backend.py --name backend-2 --port 8102 >/tmp/b2.log 2>&1 &
    B2=$!
    python3 /app/demo/demo_backend.py --name backend-3 --port 8103 >/tmp/b3.log 2>&1 &
    B3=$!
    python3 /app/demo/demo_backend.py --name backend-4 --port 8104 >/tmp/b4.log 2>&1 &
    B4=$!
    sleep 0.5
}

stop_backends() {
    kill "${B1:-}" "${B2:-}" "${B3:-}" "${B4:-}" 2>/dev/null || true
    wait "${B1:-}" "${B2:-}" "${B3:-}" "${B4:-}" 2>/dev/null || true
}

run_bench() {
    echo "=========================================="
    echo "[2/4] wrk & ab Native Benchmarks (4 Backends)"
    echo "=========================================="
    start_stack 8090 250
    ./server 8090 /tmp/ext-public 4 128 250 65536 30 10 10000 "http://127.0.0.1:8090" 5 5 \
        "http://127.0.0.1:8101,http://127.0.0.1:8102,http://127.0.0.1:8103,http://127.0.0.1:8104" >/tmp/srv_bench.log 2>&1 &
    SRV=$!
    sleep 0.5

    echo "--- wrk Static 4KB (t=4, c=50, d=10s) ---"
    wrk -t4 -c50 -d10s --latency http://127.0.0.1:8090/4kb.txt

    echo "--- wrk Static 4KB (t=4, c=100, d=10s) ---"
    wrk -t4 -c100 -d10s --latency http://127.0.0.1:8090/4kb.txt

    echo "--- wrk Proxy /api/whoami 4-Backends (t=4, c=50, d=10s) ---"
    wrk -t4 -c50 -d10s --latency http://127.0.0.1:8090/api/whoami

    echo "--- wrk Proxy /api/whoami 4-Backends (t=4, c=100, d=10s) ---"
    wrk -t4 -c100 -d10s --latency http://127.0.0.1:8090/api/whoami

    echo "--- ab Static 4KB Keep-Alive (n=20000, c=100) ---"
    ab -k -n 20000 -c 100 http://127.0.0.1:8090/4kb.txt | grep -E "Complete requests|Failed requests|Non-2xx|Requests per second|Time per request|50%|95%|99%|100%"

    echo "--- ab Proxy /api/whoami Keep-Alive (n=4000, c=50) ---"
    ab -k -n 4000 -c 50 http://127.0.0.1:8090/api/whoami | grep -E "Complete requests|Failed requests|Non-2xx|Requests per second|Time per request|50%|95%|99%|100%"

    echo "--- Server FD & RSS after wrk+ab ---"
    printf "FD count: %s\n" "$(ls /proc/$SRV/fd | wc -l)"
    grep -E "VmRSS|Threads" "/proc/$SRV/status" || true

    kill -TERM "$SRV" 2>/dev/null || true
    wait "$SRV" 2>/dev/null || true
    stop_backends
}

run_slowloris() {
    echo "=========================================="
    echo "[3/4] slowhttptest (Slowloris Attack & Recovery)"
    echo "=========================================="
    start_stack 8091 200
    ./server 8091 /tmp/ext-public 4 128 200 65536 30 10 100 "http://127.0.0.1:8091" 5 5 \
        "http://127.0.0.1:8101,http://127.0.0.1:8102,http://127.0.0.1:8103,http://127.0.0.1:8104" >/tmp/srv_slow.log 2>&1 &
    SRV=$!
    sleep 0.5

    FD_BEFORE="$(ls /proc/$SRV/fd | wc -l)"
    echo "FD before attack: $FD_BEFORE"

    # Launch Slowloris: 120 slow-header connections for 16s (server deadline is 10s -> 408)
    slowhttptest -c 120 -H -i 2 -r 60 -l 16 -t GET -u http://127.0.0.1:8091/4kb.txt \
        -o /tmp/slow_report >/tmp/slow_out.log 2>&1 &
    SLOW_PID=$!

    sleep 2
    FD_DURING="$(ls /proc/$SRV/fd | wc -l)"
    echo "FD during Slowloris attack: $FD_DURING"

    # Probe normal client survival during attack (20 requests across 10 seconds)
    OK=0
    FAIL=0
    MAX_MS=0
    for i in $(seq 1 20); do
        OUT="$(curl -s -o /dev/null -w "%{http_code} %{time_total}" --max-time 3 http://127.0.0.1:8091/4kb.txt || echo "000 3.000")"
        CODE="$(echo "$OUT" | awk '{print $1}')"
        MS="$(echo "$OUT" | awk '{printf "%.1f", $2 * 1000}')"
        if [ "$CODE" = "200" ]; then
            OK=$((OK + 1))
        else
            FAIL=$((FAIL + 1))
        fi
        sleep 0.4
    done
    echo "Normal client during Slowloris: $OK/20 HTTP 200 (fail=$FAIL)"

    wait "$SLOW_PID" || true
    echo "--- slowhttptest summary ---"
    tail -n 15 /tmp/slow_out.log || true

    # Wait past server 10s deadline + cleanup
    sleep 3
    FD_AFTER="$(ls /proc/$SRV/fd | wc -l)"
    echo "FD after attack recovery: $FD_AFTER (before=$FD_BEFORE, peak_during=$FD_DURING)"

    kill -TERM "$SRV" 2>/dev/null || true
    wait "$SRV" 2>/dev/null || true
    stop_backends
}

run_valgrind() {
    echo "=========================================="
    echo "[4/4] Valgrind Memcheck on Live Server Binary"
    echo "=========================================="
    cc -std=c11 -Wall -Wextra -Wpedantic -g -O1 -pthread -o /tmp/server_vg \
        proxy.c proxy_config.c http_body.c concurrency.c static_files.c server.c http_request.c http_response.c request_io.c

    start_stack 8092 100
    valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
        --track-origins=yes --error-exitcode=42 \
        /tmp/server_vg 8092 /tmp/ext-public 4 128 100 2048 30 10 100 "http://127.0.0.1:8092" 5 2 \
        "http://127.0.0.1:8101,http://127.0.0.1:8102,http://127.0.0.1:8103,http://127.0.0.1:8104" \
        >/tmp/vg_out.log 2>/tmp/vg_err.log &
    VG=$!
    # Wait up to 20s for Valgrind-instrumented server to start listening
    READY=0
    for _ in $(seq 1 40); do
        if curl -s -o /dev/null http://127.0.0.1:8092/ 2>/dev/null; then
            READY=1
            break
        fi
        sleep 0.5
    done
    if [ "$READY" -ne 1 ]; then
        echo "Valgrind server failed to start:"
        cat /tmp/vg_err.log
        stop_backends
        return 1
    fi

    # Exercise every major code path under Valgrind
    curl -s -o /dev/null -w "static-200:%{http_code} " http://127.0.0.1:8092/
    curl -s -o /dev/null -w "docs-301:%{http_code} " http://127.0.0.1:8092/docs
    curl -s -o /dev/null -w "symlink-403:%{http_code} " http://127.0.0.1:8092/symlink-blocked.txt
    curl -s -o /dev/null -w "missing-404:%{http_code} " http://127.0.0.1:8092/nope.txt
    curl -s -o /dev/null -w "put-501:%{http_code} " -X PUT http://127.0.0.1:8092/x
    curl -s -o /dev/null -w "post-204:%{http_code} " -X POST -d "hello" http://127.0.0.1:8092/upload
    python3 -c "
import urllib.request, urllib.error
for url, method, data in [
    ('http://127.0.0.1:8092/api/whoami', 'GET', None),
    ('http://127.0.0.1:8092/api/whoami', 'GET', None),
    ('http://127.0.0.1:8092/api/whoami', 'GET', None),
    ('http://127.0.0.1:8092/api/whoami', 'GET', None),
    ('http://127.0.0.1:8092/api/echo', 'POST', bytes(range(256))*4),
    ('http://127.0.0.1:8092/api/stream', 'GET', None),
    ('http://127.0.0.1:8092/api/redirect-internal', 'GET', None),
    ('http://127.0.0.1:8092/api/status-500', 'GET', None),
    ('http://127.0.0.1:8092/api/malformed', 'GET', None),
    ('http://127.0.0.1:8092/api/hang', 'GET', None),
    ('http://127.0.0.1:8092/big', 'POST', b'x'*2049),
]:
    req = urllib.request.Request(url, data=data, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            r.read()
            print(f'{url.split(\"/\")[-1]}:{r.status}', end=' ')
    except urllib.error.HTTPError as e:
        e.read()
        print(f'{url.split(\"/\")[-1]}:{e.code}', end=' ')
print()
"

    # Send graceful SIGTERM so Valgrind checks full cleanup on shutdown
    kill -TERM "$VG"
    VG_RC=0
    wait "$VG" || VG_RC=$?
    stop_backends

    echo "--- Valgrind Memcheck Report (exit=$VG_RC) ---"
    grep -E "HEAP SUMMARY|in use at exit|total heap usage|definitely lost|indirectly lost|possibly lost|still reachable|All heap blocks were freed|ERROR SUMMARY" /tmp/vg_err.log || cat /tmp/vg_err.log
    return "$VG_RC"
}

case "$MODE" in
    fuzz) run_fuzz ;;
    bench) run_bench ;;
    slowloris) run_slowloris ;;
    valgrind) run_valgrind ;;
    all)
        run_fuzz
        run_bench
        run_slowloris
        run_valgrind
        ;;
esac
