#!/bin/sh
# 초기 예시 설정이다. 서버 자체는 일곱 인자를 생략할 수 없다.
PORT=8080
ROOT=./public
WORKERS=4
QUEUE_LIMIT=64
CONNECTION_LIMIT=100
BODY_LIMIT=1048576
BODY_TIMEOUT=30

# 따옴표는 공백이 있는 루트 경로도 한 인자로 유지한다.
# exec로 셸을 대체하여 종료 신호가 서버에 직접 도착하게 한다.
exec ./server "$PORT" "$ROOT" "$WORKERS" "$QUEUE_LIMIT" "$CONNECTION_LIMIT" "$BODY_LIMIT" "$BODY_TIMEOUT"
