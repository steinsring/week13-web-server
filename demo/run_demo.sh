#!/bin/sh
set -e

python3 demo/demo_backend.py --name backend-1 --port 8001 &
PID1=$!
python3 demo/demo_backend.py --name backend-2 --port 8002 &
PID2=$!
python3 demo/demo_backend.py --name backend-3 --port 8003 &
PID3=$!
python3 demo/demo_backend.py --name backend-4 --port 8004 &
PID4=$!

cleanup() {
    kill "$PID1" "$PID2" "$PID3" "$PID4" 2>/dev/null || true
    wait "$PID1" "$PID2" "$PID3" "$PID4" 2>/dev/null || true
}
trap cleanup INT TERM EXIT

# 브라우저 403(심볼릭 링크 차단) 및 MIME 검증용 샘플 준비
rm -rf /tmp/demo-public
cp -r ./public /tmp/demo-public
ln -s /etc/hostname /tmp/demo-public/symlink-blocked.txt
printf 'body { color: #38bdf8; }\n' > /tmp/demo-public/sample.css
printf 'hello plain text\n' > /tmp/demo-public/sample.txt

# 백엔드 4대가 리스닝할 때까지 대기
sleep 0.6

PUBLIC_URL="${PUBLIC_URL:-http://localhost:8080}"
# 데모에서는 413 브라우저 호환(2048B) 및 504 빠른 확인(응답 타임아웃 3초)을 위해 조정
./server 8080 /tmp/demo-public 4 128 100 2048 30 10 100 "$PUBLIC_URL" 5 3 \
  http://127.0.0.1:8001,http://127.0.0.1:8002,http://127.0.0.1:8003,http://127.0.0.1:8004
