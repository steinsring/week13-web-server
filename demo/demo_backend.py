"""데모·시각화 대시보드용 비동기 HTTP/1.1 백엔드 서버 (4대 분산 + 엣지 케이스 지원)."""
import argparse
import asyncio
import hashlib
import json
import signal
import time
import urllib.parse


async def run_backend(name: str, port: int) -> None:
    seq = 0
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except NotImplementedError:
            pass

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        nonlocal seq
        try:
            raw_header = await reader.readuntil(b"\r\n\r\n")
            lines = raw_header[:-4].split(b"\r\n")
            parts = lines[0].decode("latin1").split(" ")
            if len(parts) < 3:
                return
            method, target, _ = parts[0], parts[1], parts[2]
            headers = {}
            for line in lines[1:]:
                if b":" in line:
                    k, v = line.split(b":", 1)
                    headers[k.decode("latin1").strip().lower()] = v.decode("latin1").strip()
            length = int(headers.get("content-length", "0"))
            body = await reader.readexactly(length) if length > 0 else b""

            parsed = urllib.parse.urlsplit(target)
            path = parsed.path
            query = urllib.parse.parse_qs(parsed.query)
            seq += 1
            req_id = seq
            host_hdr = headers.get("host", "")

            if path == "/api/whoami":
                payload = json.dumps({
                    "backend": name,
                    "port": port,
                    "seq": req_id,
                    "method": method,
                    "path": path,
                    "query": parsed.query,
                    "received_host": host_hdr,
                    "via": headers.get("via", ""),
                    "ts_ms": int(time.time() * 1000),
                }, ensure_ascii=False).encode("utf-8")
                resp = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: application/json; charset=utf-8\r\n"
                    b"Cache-Control: no-store\r\n"
                    b"X-Backend: " + name.encode() + b"\r\n"
                    b"Content-Length: " + str(len(payload)).encode() + b"\r\n"
                    b"Connection: close\r\n\r\n" + payload
                )
                writer.write(resp)
                await writer.drain()
                return

            if path == "/api/echo":
                digest = hashlib.sha256(body).hexdigest()[:16]
                resp = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: application/octet-stream\r\n"
                    b"Cache-Control: no-store\r\n"
                    b"X-Backend: " + name.encode() + b"\r\n"
                    b"X-Received-Bytes: " + str(len(body)).encode() + b"\r\n"
                    b"X-Sha256-Prefix: " + digest.encode() + b"\r\n"
                    b"Content-Length: " + str(len(body)).encode() + b"\r\n"
                    b"Connection: close\r\n\r\n" + body
                )
                writer.write(resp)
                await writer.drain()
                return

            if path == "/api/slow":
                ms = min(10000, max(100, int(query.get("ms", ["1400"])[0])))
                t0 = time.monotonic()
                await asyncio.sleep(ms / 1000.0)
                elapsed = round((time.monotonic() - t0) * 1000, 1)
                payload = json.dumps({
                    "backend": name,
                    "port": port,
                    "slept_ms": elapsed,
                    "seq": req_id,
                }).encode("utf-8")
                resp = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: application/json; charset=utf-8\r\n"
                    b"Cache-Control: no-store\r\n"
                    b"X-Backend: " + name.encode() + b"\r\n"
                    b"Content-Length: " + str(len(payload)).encode() + b"\r\n"
                    b"Connection: close\r\n\r\n" + payload
                )
                writer.write(resp)
                await writer.drain()
                return

            if path == "/api/hang":
                # 응답을 보내지 않고 대기 → 프록시 응답 타임아웃(504 Gateway Timeout) 유도
                await asyncio.sleep(15.0)
                return

            if path == "/api/malformed":
                # 비정상 HTTP 응답 전송 → 프록시가 502 Bad Gateway 반환하는지 검증
                writer.write(b"NOT_AN_HTTP_RESPONSE\r\n\r\n")
                await writer.drain()
                return

            if path == "/api/stream":
                writer.write(
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: text/plain; charset=utf-8\r\n"
                    b"Cache-Control: no-store\r\n"
                    b"X-Backend: " + name.encode() + b"\r\n"
                    b"Transfer-Encoding: chunked\r\n"
                    b"Connection: close\r\n\r\n"
                )
                await writer.drain()
                for i in range(1, 6):
                    chunk = f"[{name}] chunk #{i}/5 ok\n".encode("utf-8")
                    writer.write(f"{len(chunk):x}\r\n".encode("ascii") + chunk + b"\r\n")
                    await writer.drain()
                    await asyncio.sleep(0.05)
                writer.write(b"0\r\n\r\n")
                await writer.drain()
                return

            if path == "/api/redirect-internal":
                loc = f"http://127.0.0.1:{port}/api/redirect-target?from={name}"
                resp = (
                    b"HTTP/1.1 302 Found\r\n"
                    b"Location: " + loc.encode("ascii") + b"\r\n"
                    b"X-Original-Location: " + loc.encode("ascii") + b"\r\n"
                    b"X-Backend: " + name.encode() + b"\r\n"
                    b"Content-Length: 0\r\n"
                    b"Connection: close\r\n\r\n"
                )
                writer.write(resp)
                await writer.drain()
                return

            if path == "/api/redirect-target":
                payload = json.dumps({
                    "ok": True,
                    "backend": name,
                    "message": "PUBLIC_URL Location 변환 후 정상 도달",
                    "query": parsed.query,
                }, ensure_ascii=False).encode("utf-8")
                resp = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: application/json; charset=utf-8\r\n"
                    b"Content-Length: " + str(len(payload)).encode() + b"\r\n"
                    b"Connection: close\r\n\r\n" + payload
                )
                writer.write(resp)
                await writer.drain()
                return

            if path == "/api/status-500":
                payload = json.dumps({
                    "backend": name,
                    "error": "intentional_backend_500",
                    "message": "정상 백엔드의 500 응답은 502로 바꾸지 않고 그대로 전달됩니다",
                }, ensure_ascii=False).encode("utf-8")
                resp = (
                    b"HTTP/1.1 500 Internal Server Error\r\n"
                    b"Content-Type: application/json; charset=utf-8\r\n"
                    b"X-Backend: " + name.encode() + b"\r\n"
                    b"Content-Length: " + str(len(payload)).encode() + b"\r\n"
                    b"Connection: close\r\n\r\n" + payload
                )
                writer.write(resp)
                await writer.drain()
                return

            payload = json.dumps({
                "backend": name,
                "port": port,
                "method": method,
                "path": path,
                "query": parsed.query,
            }).encode("utf-8")
            resp = (
                b"HTTP/1.1 200 OK\r\n"
                b"Content-Type: application/json; charset=utf-8\r\n"
                b"X-Backend: " + name.encode() + b"\r\n"
                b"Content-Length: " + str(len(payload)).encode() + b"\r\n"
                b"Connection: close\r\n\r\n" + payload
            )
            writer.write(resp)
            await writer.drain()
        except (asyncio.IncompleteReadError, ConnectionError, ValueError):
            pass
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass

    server = await asyncio.start_server(handle, "127.0.0.1", port)
    print(f"Listening {name} on 127.0.0.1:{port}", flush=True)
    await stop.wait()
    server.close()
    await server.wait_closed()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    asyncio.run(run_backend(args.name, args.port))
