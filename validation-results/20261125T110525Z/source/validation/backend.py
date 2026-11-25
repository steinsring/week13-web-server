"""부하 검사용 결정적 HTTP 백엔드. 서버 기능에 계측 엔드포인트를 추가하지 않는다."""
import argparse
import asyncio
import json
import signal
import time
from fixtures import API_GET, API_POST


async def main(port, output):
    stats = {'requests': 0, 'get': 0, 'post': 0, 'bad_body': 0, 'connections': 0, 'active': 0}
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)

    async def client(reader, writer):
        stats['connections'] += 1
        stats['active'] += 1
        try:
            while True:
                header = await reader.readuntil(b'\r\n\r\n')
                lines = header[:-4].split(b'\r\n')
                method, target, _ = lines[0].split(b' ')
                headers = {k.lower(): v.strip().lower() for k, v in (line.split(b':', 1) for line in lines[1:])}
                length = int(headers.get(b'content-length', b'0'))
                body = await reader.readexactly(length)
                stats['requests'] += 1
                if method == b'POST':
                    stats['post'] += 1
                    if body != API_POST:
                        stats['bad_body'] += 1
                    response = body
                else:
                    stats['get'] += 1
                    response = API_GET
                close = headers.get(b'connection') == b'close'
                writer.write(b'HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: '
                             + str(len(response)).encode() + b'\r\n'
                             + (b'Connection: close\r\n' if close else b'') + b'\r\n' + response)
                await writer.drain()
                if close:
                    break
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.LimitOverrunError):
            pass
        finally:
            stats['active'] -= 1
            writer.close()
            try:
                await writer.wait_closed()
            except ConnectionError:
                pass

    async def sample():
        with open(output, 'a', buffering=1) as log:
            while not stop.is_set():
                log.write(json.dumps({'utc_ns': time.time_ns(), **stats}) + '\n')
                await asyncio.sleep(1)
            log.write(json.dumps({'utc_ns': time.time_ns(), **stats}) + '\n')

    server = await asyncio.start_server(client, '127.0.0.1', port, limit=16384)
    print(f'Listening {port}', flush=True)
    task = asyncio.create_task(sample())
    await stop.wait()
    server.close()
    await server.wait_closed()
    await task


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('port', type=int)
    parser.add_argument('output')
    args = parser.parse_args()
    asyncio.run(main(args.port, args.output))
