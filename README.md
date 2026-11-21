# C 웹 서버 · HTTP 요청 파싱

C와 POSIX 소켓으로 HTTP 요청을 받고, 지정한 파일 루트의 정적 파일을 응답한다.
응답 후 연결을 닫고 다음 연결을 기다린다.

## 코드 읽는 순서

| 파일 | 역할 |
| --- | --- |
| `server.c` | `main()` → 포트 검사 → 리스닝 소켓 준비 → 연결 수락 → `serve_client()` |
| `http_request.h` | 요청 구조체와 파서의 공개 함수 선언 |
| `http_request.c` | 조각 누적 → 요청 줄 → 헤더 → Host 검사, 요청 메모리 정리 |
| `request_io.c/.h` | 총 10초 수신 제한과 파싱 결과→HTTP 상태 코드 연결 |
| `static_files.c/.h` | 루트·경로 검사, 인코딩 해제, 파일 탐색·MIME·기본 페이지 |
| `http_response.c/.h` | `http_send_file()`·`http_send_redirect()`·`http_send_status()`로 파일·이동·오류 응답 전송 |

`serve_client()`는 요청별 구조체를 만들고, `http_receive_request()`는 받은 조각을
`http_request_feed()`에 전달한다. 수신 성공 시 `static_files_serve()`로 파일을 보내고, 수신 실패 시 상태 코드로 오류 응답을 보낸다. 이후
성공·실패 모두 `http_request_destroy()`와 `close(client)`로 정리한다. 정적 파일 모듈이 요청별 파일·디렉터리 FD도 정리한다.

문자열 포인터는 요청 구조체의 버퍼를 가리킨다. 파싱된 요청 구조체를 다른 주소로
단순 복사하지 않는다. 헤더 포인터 목록만 필요에 따라 늘리며, 중복 항목도 보존한다.

## Linux에서 직접 실행

```sh
make
./server 8080 ./public
```

포트와 정적 파일 루트는 필수 인자다. public 폴더와 index.html은 실행 전에 직접 준비한다. 직접 실행 시 Ctrl+C로 종료한다.

## 이번 단계의 범위

- HTTP/1.1 요청 줄과 모든 헤더를 줄 단위로 파싱한다.
- 경로와 쿼리는 첫 `?`로 분리하고 인코딩은 원문 그대로 저장한다.
- 메서드 이름은 저장하며 현재 GET만 지원한다. 그 밖의 메서드는 501이다.
- 헤더의 순서·이름 대소문자를 보존한다. 이름 조회는 대소문자를 무시한다.
- `Host`가 정확히 한 줄인지와 값 형식을 검사한다.
- GET 성공은 정적 파일의 HTTP 200, 파일 접근 오류와 기존 오류는 400·403·404·431·500·408·501·505로 응답 후 연결 종료. 본문에는 상태명만 보낸다.
- 요청 헤더는 최대 8192바이트다. 초과하거나 수신 중 연결이 끊기면 연결을 닫는다.
- POST 본문, 쿼리 내부 해석, 전체 URI 문법 검증은 아직 구현하지 않았다.
- `Content-Length`·`Transfer-Encoding`은 값과 중복·상호관계를 검사하지 않고 문자열로만 저장한다. 완전한 HTTP/1.1 구현은 아니다.
- 연결은 하나씩 처리한다. 연결 수락부터 헤더 수신까지 총 10초이며, 조각 데이터가 와도 기한은 늘어나지 않는다. 송신 타임아웃과 동시 처리는 아직 없다.

[요구사항](docs/project-spec.md) · [설계](docs/design-spec.md)
[파싱 요구사항](docs/http-parser-requirements.md) · [파싱 설계](docs/http-parser-design.md)
[오류 응답 요구사항](docs/error-response-requirements.md) · [오류 응답 설계](docs/error-response-design.md)

## 테스트

Linux에서는 `make test`, `make sanitize`로 파서 단위 검사를 실행한다.

서버가 localhost:8080에서 실행 중이면 별도 터미널에서 실제 TCP 동작을 검사한다.

```sh
python tests/test_server.py
```

다른 포트라면 `SERVER_TEST_PORT` 환경 변수로 지정한다.
확인한 결과는 파서 97개 검사, AddressSanitizer·UndefinedBehaviorSanitizer 및 누수 검사,
실제 TCP 기능 테스트 12개 통과다(할당 실패 서버를 함께 설정한 경우). 배열 할당 실패·확장 실패도 파서 검사에 포함한다.
응답 7종과 부분 송신·EINTR·송신 실패·기한 및 소켓 플래그 복원 검사도 통과했다. sanitizer 검사는 파서·응답·수신 모듈 범위이며, 전체 서버 부하 테스트는 실행하지 않았다.

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
