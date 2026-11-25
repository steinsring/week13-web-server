#ifndef HTTP_BODY_H
#define HTTP_BODY_H
#include "http_request.h"

typedef struct {
    unsigned char* bytes;
    size_t length;
    size_t received;
} HttpBody;

/* 원문 헤더는 바꾸지 않는다. 성공 0, 실패는 최종 HTTP 상태 코드다. */
int http_body_validate(const HttpRequest* request, size_t limit,
    size_t* length, int* expect_continue);
int http_body_allocate(HttpBody* body, size_t length);
void http_body_feed(HttpBody* body, const void* bytes, size_t length);
void http_body_destroy(HttpBody* body);
#endif
