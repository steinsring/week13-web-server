#include "http_body.h"
#include <assert.h>
#include <stdio.h>
#undef malloc
#include <stdlib.h>
#include <string.h>
static int fail_allocation;
void *test_body_malloc(size_t size) { return fail_allocation ? NULL : malloc(size); }

static int validate(const char *headers, const char *method, size_t limit, size_t *length, int *expect)
{
    char wire[2048];
    int n = snprintf(wire, sizeof(wire), "%s / HTTP/1.1\r\nHost: localhost\r\n%s\r\n", method, headers);
    HttpRequest r; http_request_init(&r);
    assert(http_request_feed(&r, wire, (size_t)n) == HTTP_PARSE_COMPLETE);
    int status = http_body_validate(&r, limit, length, expect);
    http_request_destroy(&r);
    return status;
}
int main(void)
{
    size_t length; int expect;
    assert(!validate("Content-Length: 5\r\nExpect: 100-Continue\r\n", "POST", 5, &length, &expect));
    assert(length == 5 && expect);
    assert(validate("Content-Length: 6\r\n", "POST", 5, &length, &expect) == 413);
    const char *bad[] = {"", "+1", "-1", "1,1", "1x", "184467440737095516160"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) {
        char header[100]; snprintf(header, sizeof(header), "Content-Length: %s\r\n", bad[i]);
        assert(validate(header, "POST", 10, &length, &expect) == 400);
    }
    assert(validate("Content-Length: 0\r\nContent-Length: 0\r\n", "POST", 10, &length, &expect) == 400);
    assert(validate("Content-Length: 0\r\nTransfer-Encoding: chunked\r\n", "POST", 10, &length, &expect) == 400);
    assert(validate("Transfer-Encoding: gzip; a=\"x,y\", chunked\r\n", "POST", 10, &length, &expect) == 501);
    assert(validate("Transfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n", "POST", 10, &length, &expect) == 501);
    const char *bad_te[] = {"gzip", "", "chunked, gzip", "chunked, chunked", "gzip,", "gzip; x", "chunked; a=b", "gzip; a=\"unterminated"};
    for (size_t i = 0; i < sizeof(bad_te)/sizeof(*bad_te); ++i) {
        char header[128]; snprintf(header, sizeof(header), "Transfer-Encoding: %s\r\n", bad_te[i]);
        assert(validate(header, "POST", 10, &length, &expect) == 400);
    }
    assert(validate("Content-Length: 6\r\n", "GET", 5, &length, &expect) == 400);
    assert(validate("Expect: other\r\n", "POST", 5, &length, &expect) == 417);
    assert(validate("", "PUT", 5, &length, &expect) == 501);

    /* 헤더 경계가 어느 위치에서 나뉘어도 NUL/개행을 문자열로 다루지 않는다. */
    const char prefix[] = "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\n";
    const unsigned char payload[] = {'a', 0, '\r', '\n', 255, 'X', 'Y'};
    unsigned char wire[sizeof(prefix) + sizeof(payload)];
    memcpy(wire, prefix, sizeof(prefix)-1);
    memcpy(wire + sizeof(prefix)-1, payload, sizeof(payload));
    size_t total = sizeof(prefix)-1 + sizeof(payload);
    for (size_t split = 0; split <= total; ++split) {
        HttpRequest r; http_request_init(&r); HttpBody body = {0};
        size_t positions[] = {0, split, total};
        int allocated = 0;
        for (unsigned part = 0; part < 2; ++part) {
            size_t start = positions[part], size = positions[part+1]-start, consumed = 0;
            if (!allocated) {
                HttpParseResult parsed = http_request_feed_bytes(&r, (const char*)wire + start, size, &consumed);
                if (parsed == HTTP_PARSE_INCOMPLETE) continue;
                assert(parsed == HTTP_PARSE_COMPLETE);
                assert(!http_body_allocate(&body, 5)); allocated = 1;
            }
            http_body_feed(&body, wire + start + consumed, size - consumed);
        }
        assert(body.length == 5 && body.received == 5 && !memcmp(body.bytes, payload, 5));
        http_body_destroy(&body); http_request_destroy(&r);
    }
    HttpBody body = {0}; fail_allocation = 1;
    assert(http_body_allocate(&body, 10) == 500); http_body_destroy(&body);
    assert(!http_body_allocate(&body, 0) && !body.bytes); http_body_destroy(&body);
    puts("PASS: body validation, exact binary boundary at every split, bounded copy and allocation failure");
}
