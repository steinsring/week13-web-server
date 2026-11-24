#include "http_body.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void skip_space(const char** p)
{
    while (**p == ' ' || **p == '\t')
        ++*p;
}
static int token_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c && strchr("!#$%&'*+-.^_`|~", c));
}
static int read_token(const char** p)
{
    const char* start = *p;
    while (token_char((unsigned char)**p))
        ++*p;
    return *p != start;
}
static int read_parameter_value(const char** p)
{
    if (**p != '"')
        return read_token(p);
    ++*p;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)*(*p)++;
        if (c == '\\') {
            c = (unsigned char)**p;
            if (!c)
                return 0;
            ++*p;
        }
        if ((c < 32 && c != '\t') || c == 127)
            return 0;
    }
    if (**p != '"')
        return 0;
    ++*p;
    return 1;
}

/* 여러 TE 줄은 한 목록처럼 검사한다. chunked는 한 번, 맨 끝에만 가능하다.
 * 해석 가능한 문법인지 확인할 뿐, 이번 단계에 디코더는 넣지 않는다. */
static int transfer_encoding_status(const HttpRequest* request)
{
    size_t cursor = 0;
    const HttpHeader* header;
    int chunked = 0;
    while ((header = http_request_next_header(request, "Transfer-Encoding", &cursor))) {
        const char* p = header->value;
        for (;;) {
            skip_space(&p);
            const char* start = p;
            if (chunked || !read_token(&p))
                return 400;
            int current_chunked = (size_t)(p - start) == 7 && !strncasecmp(start, "chunked", 7);
            skip_space(&p);
            while (*p == ';') {
                if (current_chunked)
                    return 400;
                ++p;
                skip_space(&p);
                if (!read_token(&p))
                    return 400;
                skip_space(&p);
                if (*p++ != '=')
                    return 400;
                skip_space(&p);
                if (!read_parameter_value(&p))
                    return 400;
                skip_space(&p);
            }
            chunked = current_chunked;
            if (!*p)
                break;
            if (*p++ != ',')
                return 400;
        }
    }
    return chunked ? 501 : 400;
}

static int content_length(const char* value, size_t* length)
{
    const char* p = value;
    skip_space(&p);
    if (*p < '0' || *p > '9')
        return 400;
    size_t n = 0;
    do {
        unsigned digit = (unsigned)(*p++ - '0');
        if (n > (SIZE_MAX - digit) / 10)
            return 400;
        n = n * 10 + digit;
    } while (*p >= '0' && *p <= '9');
    skip_space(&p);
    if (*p)
        return 400;
    *length = n;
    return 0;
}

static int validate_expect(const HttpRequest* request, int* expect)
{
    size_t cursor = 0;
    const HttpHeader* header;
    *expect = 0;
    while ((header = http_request_next_header(request, "Expect", &cursor))) {
        const char* p = header->value;
        skip_space(&p);
        const char* end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t'))
            --end;
        if ((size_t)(end - p) != 12 || strncasecmp(p, "100-continue", 12))
            return 417;
        *expect = 1;
    }
    return 0;
}

int http_body_validate(const HttpRequest* request, size_t limit,
    size_t* length, int* expect_continue)
{
    size_t cursor = 0;
    const HttpHeader* cl = http_request_next_header(request, "Content-Length", &cursor);
    int duplicate = http_request_next_header(request, "Content-Length", &cursor) != NULL;
    cursor = 0;
    int te = http_request_next_header(request, "Transfer-Encoding", &cursor) != NULL;
    *length = 0;
    *expect_continue = 0;
    if (duplicate || (cl && te))
        return 400;
    if (te)
        return transfer_encoding_status(request);
    if (cl && content_length(cl->value, length))
        return 400;
    if (!strcmp(request->method, "GET")) {
        if (*length)
            return 400;
    } else if (!strcmp(request->method, "POST")) {
        if (*length > limit)
            return 413;
    } else
        return 501;
    return validate_expect(request, expect_continue);
}

int http_body_allocate(HttpBody* body, size_t length)
{
    body->length = length;
    body->received = 0;
    body->bytes = length ? malloc(length) : NULL;
    return length && !body->bytes ? 500 : 0;
}
void http_body_feed(HttpBody* body, const void* bytes, size_t length)
{
    size_t remaining = body->length - body->received;
    if (length > remaining)
        length = remaining;
    if (length)
        memcpy(body->bytes + body->received, bytes, length);
    body->received += length;
}
void http_body_destroy(HttpBody* body)
{
    free(body->bytes);
    memset(body, 0, sizeof(*body));
}
