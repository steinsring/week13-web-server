# C 웹 서버 · 동시 정적 파일 서빙

C와 POSIX 소켓으로 HTTP 요청을 받고, 지정한 파일 루트의 정적 파일을 응답한다.
여러 연결을 동시에 처리하며 HTTP/1.1 연결을 재사용해 요청을 순서대로 처리한다.

## 코드 읽는 순서

| 파일 | 역할 |
| --- | --- |
| `server.c` | `main()` → 필수 인자·자원 검사 → 리스닝 소켓 준비 → 이벤트 루프 |
| `concurrency.c/.h` | epoll 연결 관리·시간 제한·송신, pthread 파일 작업 큐와 종료 |
| `http_body.c/.h` | CL·TE·Expect 검증, 본문 메모리 확보·길이 기반 복사·해제 |
| `http_request.h` | 요청 구조체와 파서의 공개 함수 선언 |
| `http_request.c` | 조각 누적 → 요청 줄 → 헤더 → Host 검사, 요청 메모리 정리 |
| `request_io.c/.h` | 이전 순차 수신 API와 해당 회귀 검사 |
| `static_files.c/.h` | 루트·경로 검사, 인코딩 해제, 파일 탐색·MIME·기본 페이지 |
| `http_response.c/.h` | 상태명·응답 헤더 생성, 이전 동기 전송 API |

이벤트 루프가 `accept()` → `recv()` → `http_request_feed()`를 수행한다.
파일 작업은 스레드가 `static_files_prepare()`로 준비하고 한 조각씩 읽는다.
이벤트 루프만 `send()`를 호출하며, 조각을 모두 보낸 뒤 다음 읽기를 요청한다.
연결 종료 시 요청과 파일을 정리한다. 실행 중 파일 작업이 있으면 완료까지 메모리와 파일 FD를 유지한다.
문자열 포인터는 요청 구조체의 버퍼를 가리킨다. 파싱된 요청 구조체를 다른 주소로
단순 복사하지 않는다. 헤더 포인터 목록만 필요에 따라 늘리며, 중복 항목도 보존한다.

## Docker 실행

Docker가 실행 중이면 저장소 루트에서 실행한다. 별도 GCC 설치나 파일 준비 없이 기본 페이지를 확인할 수 있다.

```sh
docker build -t week13-web-server .
docker run --rm --name week13-web-server -p 127.0.0.1:8080:8080 week13-web-server
```

브라우저에서 http://localhost:8080/을 연다. 기본 파일 루트는 이미지에 포함된 `/app/public`이다.
종료는 다른 터미널에서 `docker stop --timeout 15 week13-web-server`를 실행한다.
Windows PowerShell에서 요청을 확인하려면 `curl.exe -i http://localhost:8080/`을 사용한다.

자신의 public 폴더를 쓰려면 다음처럼 읽기 전용으로 연결한다.

```sh
docker run --rm --name week13-web-server --mount "type=bind,source=$PWD/public,target=/app/public,readonly" -p 127.0.0.1:8080:8080 week13-web-server
```

기반 GCC/Linux 이미지는 digest로 고정한다. 같은 아키텍처에서는 동일한 기반 이미지와 빌드 명령을 사용한다.
호스트 커널·CPU 아키텍처와 실행 중 변경하는 파일까지 동일해지는 것은 아니다.

VS Code에서는 Dev Containers 확장을 설치하고 `Dev Containers: Reopen in Container`를 선택한다.
컨테이너 터미널에서 `./server 8080 ./public 4 64 100 1048576 30 10 100`으로 실행한다. 서버는 자동 실행하지 않는다.

## Linux에서 직접 실행

```sh
make
./server 8080 ./public 4 64 100 1048576 30 10 100
```

포트·정적 파일 루트·작업 스레드 수·대기 작업 수 상한·유지 연결 수 상한·본문 최대 바이트·본문 총수신 초·keep-alive idle 초·최대 요청 수는 모두 필수 인자다. 위 숫자는 실행 예시이며 서버 기본값이 아니다. Linux epoll을 사용하므로 macOS에서는 Docker Linux로 실행한다. public 폴더와 index.html은 실행 전에 직접 준비한다. 직접 실행 시 Ctrl+C로 종료한다.

## 이번 단계의 범위

- HTTP/1.1 요청 줄과 모든 헤더를 줄 단위로 파싱한다.
- 경로와 쿼리는 첫 `?`로 분리하고 인코딩은 원문 그대로 저장한다.
- GET은 정적 파일, POST는 본문 수신 완료 후 204로 응답한다. 그 밖의 메서드는 501이다.
- 헤더의 순서·이름 대소문자를 보존한다. 이름 조회는 대소문자를 무시한다.
- `Host`가 정확히 한 줄인지와 값 형식을 검사한다.
- GET 성공은 정적 파일의 HTTP 200, 파일 접근 오류와 기존 오류는 400·403·404·431·500·408·501·503·505로 응답한다. 완전 수신한 403·404는 재사용하고 나머지 오류는 닫는다. 본문에는 상태명만 보낸다.
- 요청 헤더는 최대 8192바이트다. 초과 시 431을 시도하고, 수신 중 연결이 끊기면 닫는다.
- POST 본문은 길이와 원문 바이트로 보관한다. JSON·폼·쿼리 내용 해석과 전체 URI 문법 검증은 추가하지 않았다.
- Content-Length를 검증하고 길이만큼만 받는다. 중복 CL·CL+TE는 400, 유효 chunked 전송은 미지원 501이다. GET의 양수 CL은 400이다.
- epoll 이벤트 루프가 소켓을 관리하고 고정 개수의 pthread가 파일 작업을 처리한다.
- 첫 파일 작업의 대기열이 꽉 차면 503이다. 후속 읽기는 연결별로 기다리고 빈자리가 나면 오래 기다린 순서로 제출한다.
- 연결 상한이면 accept 감시를 잠시 중단한다. 실행 중 작업이 남은 닫힌 연결도 정리될 때까지 슬롯을 유지한다.
- 헤더 수신 총 10초, 송신 무진행 10초, 파일 작업 대기 10초를 각각 적용한다. 실행 중 파일 I/O 시간은 대기 시간에서 제외한다.
- SIGINT·SIGTERM은 새 연결 수락을 멈추고 최대 10초 마무리한다. 기한이 지나면 프로세스를 종료해 남은 스레드와 자원을 OS가 회수한다.

[요구사항](docs/project-spec.md) · [설계](docs/design-spec.md)
[파싱 요구사항](docs/http-parser-requirements.md) · [파싱 설계](docs/http-parser-design.md)
[오류 응답 요구사항](docs/error-response-requirements.md) · [오류 응답 설계](docs/error-response-design.md)

## 테스트

Linux에서는 `make test`, `make sanitize`로 파서 단위 검사를 실행한다.
Docker에서도 실행할 수 있다.

```sh
docker run --rm week13-web-server make test sanitize
```

서버가 localhost:8080에서 실행 중이면 별도 터미널에서 실제 TCP 동작을 검사한다.

```sh
python tests/test_server.py
```

다른 포트라면 `SERVER_TEST_PORT` 환경 변수로 지정한다.
단위 검사는 파서 97개, 정적 파일 2400개와 응답·동시 상태 검사를 포함하며 ASan·UBSan·누수 검사도 통과했다.
기존 오류 TCP 12개, 정적 파일 TCP 15개와 동시 TCP 10개를 통과했다. 동시 검사는 느린 수신·송신, 큐 포화,
작업 대기 제한과 실행 시간 제외, 연결 상한·재개, 단절·FD 재사용, 정상·기한 종료를 포함한다.
동시 서버 ASan·UBSan·누수 검사는 병렬 요청·단절·큐 포화·정상 종료 4개 시나리오를 통과했다.
TSan은 런타임의 `unexpected memory mapping` 오류로 시작하지 못해 경합 검증을 완료하지 못했다.
강제 프로세스 종료 경로는 정상 종료 시의 누수 검사와 구분한다. 처리량 부하 측정은 실행하지 않았다.

동시 TCP 검사는 테스트 전용 파일 열기 지연 래퍼로 실행한다.

```sh
cc -std=c11 -Wall -Wextra -Wpedantic -O1 -g -pthread -I. http_body.c concurrency.c static_files.c server.c http_request.c http_response.c request_io.c tests/slow_file.c -Wl,--wrap=openat -o /tmp/server-concurrency-test
TEST_SERVER=/tmp/server-concurrency-test python3 tests/test_concurrency.py
```
실제 TCP 테스트의 기본 포트는 8080이다. 500 할당 실패 검사는 SERVER_ALLOC_FAIL_PORT 환경 변수로 테스트용 서버 포트를 지정해야 실행되며, 지정하지 않으면 해당 검사만 건너뛴다.

## 정적 파일 서빙

- `/`와 `/docs/`는 해당 디렉터리의 index.html을 보낸다. 없으면 404다.
- 실제 디렉터리 `/docs`는 301로 `/docs/`에 이동한다. 쿼리도 보존한다.
- 파일 경로는 퍼센트 인코딩을 한 번만 해제한다. 쿼리는 파일명에서 제외한다.
- 루트 탈출, 중간·최종 심볼릭 링크, 접근 권한 및 비정규 파일은 403이다.
- 파일은 고정 버퍼로 읽어 보내며 바이너리와 빈 파일도 원본 그대로 전송한다.
- 알 수 없는 확장자는 application/octet-stream이다.
- Linux에서 `python3 tests/test_static_files.py`로 임시 루트·서버를 만든다.
  권한 검사를 위해 일반 사용자로 실행한다. `make test`, `make sanitize`에도 정적 모듈 검사가 포함된다.

기존 TCP 오류 테스트는 index.html 내용이 정확히 hello world(줄바꿈 없음)인 서버를 대상으로 한다.
[정적 파일 요구사항](docs/static-files-requirements.md) · [정적 파일 설계](docs/static-files-design.md)

[동시 처리 요구사항](docs/concurrency-requirements.md) · [동시 처리 설계](docs/concurrency-design.md)

## POST 본문 수신

`sh ./start.sh`로도 실행할 수 있다. 스크립트의 PORT·ROOT·WORKERS·QUEUE_LIMIT·CONNECTION_LIMIT·BODY_LIMIT·BODY_TIMEOUT·KEEP_ALIVE_TIMEOUT·KEEP_ALIVE_MAX_REQUESTS를 수정하면 아홉 인자로 전달한다. 공백이 있는 ROOT도 한 인자로 전달한다. Docker 이미지에서는 실행 권한을 설정하므로 `./start.sh`도 사용할 수 있다.

모든 POST 경로는 본문 완료 후 `204 No Content`로 응답한다. 응답 본문·Content-Length·Transfer-Encoding은 없다. 본문 선언 길이 뒤의 데이터는 다음 요청으로 보관하며 현재 응답 뒤에 처리한다.
본문 총기한은 헤더 완료부터 시작하고 수신 조각이나 100 Continue로 늘어나지 않는다. 크기 초과는 413, 미달 EOF는 400, 기한 초과는 408, 본문 할당 실패는 500이다.
Expect: 100-continue는 사전 검사를 마치고 필요할 때만 보낸다. 그 밖의 기대 값은 417이다. 임시 응답과 최종 응답의 부분 송신 상태는 별도로 관리한다.

```sh
python3 tests/test_post_body.py
```

[POST 요구사항](docs/post-body-requirements.md) · [POST 설계](docs/post-body-design.md)

## Keep-alive

HTTP/1.1은 기본 유지한다. Connection 헤더의 어느 쉼표 토큰에든 close가 있으면 종료를 우선한다. 유지 응답에는 Connection: close를 넣지 않는다. 204와 100에는 길이 헤더를 넣지 않는다.

현재 최종 응답을 끝까지 보낸 뒤에만 다음 요청을 시작한다. 한 recv의 잔여 데이터는 연결별 4096바이트 버퍼에 보관하고 응답 중 추가 읽기를 멈춘다. 이전 요청의 헤더·본문·파일은 정리하면서 소켓·횟수·잔여 데이터는 유지한다.

다음 요청이 없으면 설정한 idle 기한 뒤 응답 없이 닫는다. 첫 바이트를 읽으면 별도의 헤더 10초 기한을 시작한다. N번째 최종 응답은 close이며 100은 횟수에 넣지 않는다. start.sh의 초기 idle 10초·최대 100회는 수정 가능한 설정이다. 종료 시 idle 연결은 바로 닫고, 진행 중 요청만 기존 유예 내에 마무리한다.

```sh
python3 tests/test_keep_alive.py
```

기존 EOF 기반 TCP 검사는 최대 요청 수를 1로 설정해 이전 프로토콜 응답을 검사한다. 새 keep-alive 검사는 기본 유지와 응답 경계로 다음 요청을 구분한다.
[Keep-alive 요구사항](docs/keep-alive-requirements.md) · [설계](docs/keep-alive-design.md)

이번 keep-alive 단계 검증: 단위 및 sanitizer 검사, 새 TCP 13개와 기존 오류 12개·정적 15개·동시 10개가 통과했다. 전체 서버 ASan·UBSan·누수 검사에서는 keep-alive 13개와 POST 13개가 통과했다. Docker 이미지의 start.sh와 curl 연결 재사용도 확인했다.
