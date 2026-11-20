#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>

#define HTTP_HEADER_LIMIT 8192

typedef struct {
    char *name;
    char *value;
} HttpHeader;

typedef enum {
    HTTP_PARSE_INCOMPLETE = 0,
    HTTP_PARSE_COMPLETE = 1,
    HTTP_PARSE_INVALID = -1,
    HTTP_PARSE_TOO_LARGE = -2,
    HTTP_PARSE_NO_MEMORY = -3
} HttpParseResult;

/* 요청 하나가 자기 버퍼와 헤더 목록을 소유한다.
 * 문자열은 buffer 안을 가리킨다. 파싱 후 이 구조체를 단순 값 복사하거나
 * 이동하면 포인터가 예전 버퍼를 가리키므로, 처리 중 주소를 유지한다.
 */
typedef struct {
    char buffer[HTTP_HEADER_LIMIT + 1];
    char *method;
    char *path;
    char *query;                 /* ?가 없으면 NULL, /path?이면 빈 문자열 */
    char *version;
    HttpHeader *headers;
    size_t header_count;
    size_t header_capacity;
    size_t used;                /* 누적 바이트 수 */
    size_t line_start;          /* 다음에 파싱할 줄의 시작 위치 */
    int request_line_done;
    HttpParseResult result;
} HttpRequest;

void http_request_init(HttpRequest *request);
void http_request_destroy(HttpRequest *request);

/* 받은 조각을 공급한다. 완성 전 INCOMPLETE, 헤더 끝 COMPLETE.
 * 헤더 뒤 본문은 이번 단계에서 처리하지 않는다.
 */
HttpParseResult http_request_feed(HttpRequest *request, const char *data, size_t length);

/* cursor를 0으로 시작해 반복 호출하면 같은 이름의 헤더를 순서대로 찾는다.
 * 이름은 대소문자를 무시한다. 더 없으면 NULL.
 */
const HttpHeader *http_request_next_header(const HttpRequest *request,
                                         const char *name, size_t *cursor);

#endif
