# C 웹 서버 · HTTP 요청 파싱

C와 POSIX 소켓으로 HTTP 요청을 받고, TCP 연결로 `hello world`를 응답한다.
응답 후 연결을 닫고 다음 연결을 기다린다.

## 코드 읽는 순서

| 파일 | 역할 |
| --- | --- |
| `server.c` | `main()` → 포트 검사 → 리스닝 소켓 준비 → 연결 수락 → `serve_client()` |
| `http_request.h` | 요청 구조체와 파서의 공개 함수 선언 |
| `http_request.c` | 조각 누적 → 요청 줄 → 헤더 → Host 검사, 요청 메모리 정리 |
| `http_response.c/.h` | `http_send_hello()`로 고정 응답 전체 전송 |

`serve_client()`는 요청별 구조체를 만들고, `receive_request()`는 받은 조각을
`http_request_feed()`에 전달한다. 파싱 성공 시 `http_send_hello()`를 호출하고,
성공·실패 모두 `http_request_destroy()`와 `close(client)`로 정리한다.

문자열 포인터는 요청 구조체의 버퍼를 가리킨다. 파싱된 요청 구조체를 다른 주소로
단순 복사하지 않는다. 헤더 포인터 목록만 필요에 따라 늘리며, 중복 항목도 보존한다.

## Linux에서 직접 실행

```sh
make
./server 8080
```

포트 인자를 생략하면 8080을 사용한다. 직접 실행 시 Ctrl+C로 종료한다.
브라우저에서 http://localhost:8080/에 접속하거나 curl로 확인할 수 있다.

## 이번 단계의 범위

- HTTP/1.1 요청 줄과 모든 헤더를 줄 단위로 파싱한다.
- 경로와 쿼리는 첫 `?`로 분리하고 인코딩은 원문 그대로 저장한다.
- 메서드 이름은 저장하되 기능 실행 여부는 아직 판단하지 않는다.
- 헤더의 순서·이름 대소문자를 보존한다. 이름 조회는 대소문자를 무시한다.
- `Host`가 정확히 한 줄인지와 값 형식을 검사한다.
- 파싱 성공 시 같은 HTTP 200 응답, 실패 시 오류 HTTP 응답 없이 연결 종료.
- 요청 헤더는 최대 8192바이트다. 초과하거나 수신 중 연결이 끊기면 연결을 닫는다.
- POST 본문, 쿼리 내부 해석, 전체 URI 문법 검증은 아직 구현하지 않았다.
- `Content-Length`·`Transfer-Encoding`은 값과 중복·상호관계를 검사하지 않고 문자열로만 저장한다. 완전한 HTTP/1.1 구현은 아니다.
- 연결은 하나씩 처리한다. 수신 타임아웃이 없어 불완전한 요청이 연결을 유지하면 다음 연결 처리가 지연된다.

[요구사항](docs/project-spec.md) · [설계](docs/design-spec.md)
[파싱 요구사항](docs/http-parser-requirements.md) · [파싱 설계](docs/http-parser-design.md)

## 테스트

Linux에서는 `make test`, `make sanitize`로 파서 단위 검사를 실행한다.

서버가 localhost:8080에서 실행 중이면 별도 터미널에서 실제 TCP 동작을 검사한다.

```sh
python tests/test_server.py
```

다른 포트라면 `SERVER_TEST_PORT` 환경 변수로 지정한다.
확인한 결과는 파서 96개 검사, AddressSanitizer·UndefinedBehaviorSanitizer 및 누수 검사,
실제 TCP 기능 테스트 8개 통과다. 배열 할당 실패·확장 실패도 파서 검사에 포함한다.
sanitizer 검사는 파서 범위이며, 전체 서버 부하 테스트는 실행하지 않았다.
