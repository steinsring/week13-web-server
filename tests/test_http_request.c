/* 테스트 빌드에서만 realloc을 감싸 할당 실패를 재현한다. */
#ifdef realloc
#undef realloc
#endif
#include "http_request.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int allocations_until_failure = -1;
static size_t checks;

void *test_realloc(void *pointer, size_t size)
{
    if (allocations_until_failure == 0)
        return NULL;
    if (allocations_until_failure > 0)
        --allocations_until_failure;
    return realloc(pointer, size);
}

static void expect(const char *input, HttpParseResult expected)
{
    HttpRequest request;
    http_request_init(&request);
    HttpParseResult result = http_request_feed(&request, input, strlen(input));
    if (result != expected) {
        fprintf(stderr, "Unexpected result %d (expected %d): %s\n", result, expected, input);
        abort();
    }
    http_request_destroy(&request);
    ++checks;
}

static void test_saved_values(void)
{
    const char *input =
        "CUSTOM /hello%20world?q=a%20b&next=? HTTP/1.1\r\n"
        "hOsT: localhost:8080\r\n"
        "X-Name: \t hello world \t \r\n"
        "Accept:text/html\r\n"
        "accept: application/json\r\n"
        "X-Inside: one\t two:three\r\n"
        "X-Empty: \t\r\n"
        "Content-Length: abc\r\n"
        "Content-Length: 2\r\n"
        "Transfer-Encoding: anything\r\n\r\n";
    HttpRequest request;
    http_request_init(&request);
    assert(http_request_feed(&request, input, strlen(input)) == HTTP_PARSE_COMPLETE);
    assert(strcmp(request.method, "CUSTOM") == 0);
    assert(strcmp(request.path, "/hello%20world") == 0);
    assert(strcmp(request.query, "q=a%20b&next=?") == 0);
    assert(strcmp(request.version, "HTTP/1.1") == 0);
    assert(request.method >= request.buffer && request.method < request.buffer + request.used);
    assert(request.header_count == 9);
    assert(strcmp(request.headers[0].name, "hOsT") == 0);
    assert(strcmp(request.headers[1].value, "hello world") == 0);
    assert(strcmp(request.headers[4].value, "one\t two:three") == 0);
    assert(strcmp(request.headers[5].value, "") == 0);
    size_t cursor = 0;
    const HttpHeader *header = http_request_next_header(&request, "ACCEPT", &cursor);
    assert(header && strcmp(header->name, "Accept") == 0);
    assert(strcmp(header->value, "text/html") == 0);
    header = http_request_next_header(&request, "ACCEPT", &cursor);
    assert(header && strcmp(header->name, "accept") == 0);
    assert(strcmp(header->value, "application/json") == 0);
    assert(http_request_next_header(&request, "ACCEPT", &cursor) == NULL);
    http_request_destroy(&request);
    assert(request.headers == NULL && request.method == NULL && request.used == 0);
    ++checks;
}

static void test_fragments(void)
{
    const char *input = "GET /search?q=cat HTTP/1.1\r\nHost: localhost\r\nX: a\r\n\r\n";
    size_t length = strlen(input);
    /* 가능한 모든 두 조각 경계와, 한 바이트씩 도착하는 경우를 확인한다. */
    for (size_t split = 0; split <= length; ++split) {
        HttpRequest request;
        http_request_init(&request);
        assert(http_request_feed(&request, input, split) ==
               (split == length ? HTTP_PARSE_COMPLETE : HTTP_PARSE_INCOMPLETE));
        assert(http_request_feed(&request, input + split, length - split) == HTTP_PARSE_COMPLETE);
        assert(strcmp(request.path, "/search") == 0);
        assert(strcmp(request.query, "q=cat") == 0);
        assert(strcmp(request.headers[1].value, "a") == 0);
        http_request_destroy(&request);
        ++checks;
    }
    HttpRequest request;
    http_request_init(&request);
    for (size_t i = 0; i < length; ++i)
        assert(http_request_feed(&request, input + i, 1) ==
               (i + 1 == length ? HTTP_PARSE_COMPLETE : HTTP_PARSE_INCOMPLETE));
    http_request_destroy(&request);
    ++checks;
}

static void test_query_and_independence(void)
{
    HttpRequest a, b;
    http_request_init(&a);
    http_request_init(&b);
    const char *first = "GET /one HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const char *second = "GET /two? HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(http_request_feed(&a, first, strlen(first)) == HTTP_PARSE_COMPLETE);
    assert(http_request_feed(&b, second, strlen(second)) == HTTP_PARSE_COMPLETE);
    assert(a.query == NULL);
    assert(b.query && *b.query == '\0');
    assert(strcmp(a.path, "/one") == 0 && strcmp(b.path, "/two") == 0);
    http_request_destroy(&a);
    assert(strcmp(b.path, "/two") == 0);
    http_request_destroy(&b);
    ++checks;
}

static void test_growth_and_allocation_failure(void)
{
    char input[4096];
    size_t used = (size_t)snprintf(input, sizeof(input), "GET / HTTP/1.1\r\nHost: localhost\r\n");
    for (int i = 0; i < 100; ++i)
        used += (size_t)snprintf(input + used, sizeof(input) - used, "X-%d: value-%d\r\n", i, i);
    memcpy(input + used, "\r\n", 3);
    used += 2;
    HttpRequest request;
    http_request_init(&request);
    assert(http_request_feed(&request, input, used) == HTTP_PARSE_COMPLETE);
    assert(request.header_count == 101);
    assert(strcmp(request.headers[100].value, "value-99") == 0);
    assert(strcmp(request.headers[0].name, "Host") == 0);
    http_request_destroy(&request);
    ++checks;

    for (int fail_after = 0; fail_after < 5; ++fail_after) {
        allocations_until_failure = fail_after;
        http_request_init(&request);
        assert(http_request_feed(&request, input, used) == HTTP_PARSE_NO_MEMORY);
        http_request_destroy(&request);
        ++checks;
    }
    allocations_until_failure = -1;
}

static void test_size_limit(void)
{
    const char *prefix = "GET / HTTP/1.1\r\nHost: localhost\r\nX: ";
    for (size_t size = HTTP_HEADER_LIMIT; size <= HTTP_HEADER_LIMIT + 1; ++size) {
        char *input = malloc(size + 1);
        assert(input);
        size_t prefix_size = strlen(prefix);
        memcpy(input, prefix, prefix_size);
        memset(input + prefix_size, 'a', size - prefix_size - 4);
        memcpy(input + size - 4, "\r\n\r\n", 4);
        input[size] = '\0';
        HttpRequest request;
        http_request_init(&request);
        assert(http_request_feed(&request, input, size) ==
               (size == HTTP_HEADER_LIMIT ? HTTP_PARSE_COMPLETE : HTTP_PARSE_TOO_LARGE));
        http_request_destroy(&request);
        free(input);
        ++checks;
    }
}

int main(void)
{
    const char *invalid[] = {
        "GET /\r\nHost: localhost\r\n\r\n",
        "GET  / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET / HTTP/1.1 extra\r\nHost: localhost\r\n\r\n",
        "GET\t/ HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",
        "G(ET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "GET / HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost\r\nhost: localhost\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : localhost\r\n\r\n",
        "GET / HTTP/1.1\r\nHost localhost\r\n\r\n",
        "GET / HTTP/1.1\r\n: value\r\nHost: localhost\r\n\r\n",
        "GET / HTTP/1.1\r\nBad Name: value\r\nHost: localhost\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: local host\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost:abc\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: localhost:\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: \r\n\r\n",
        "GET / HTTP/1.1\r\nHost: [::1\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: [not-ipv6]\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: [::1]junk\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: bad%xy\r\n\r\n",
        "GET / HTTP/1.1\nHost: localhost\n\n",
        "GET / HTTP/1.1\rX",
        "GET / HTTP/1.1\r\nHost: localhost\r\n folded\r\n\r\n"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        expect(invalid[i], HTTP_PARSE_INVALID);
    expect("GET / HTTP/1.1\r\nHost:localhost\r\n\r\n", HTTP_PARSE_COMPLETE);
    expect("GET / HTTP/1.1\r\nHost: [::1]:8080\r\n\r\n", HTTP_PARSE_COMPLETE);
    expect("GET / HTTP/1.1\r\nHost: [2001:db8::1]\r\n\r\n", HTTP_PARSE_COMPLETE);
    expect("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", HTTP_PARSE_COMPLETE);
    expect("GET / HTTP/1.1\r\nHost: local%68ost\r\n\r\n", HTTP_PARSE_COMPLETE);
    expect("GET / HTTP/1.1\r\nHost: localhost", HTTP_PARSE_INCOMPLETE);

    HttpRequest request;
    http_request_init(&request);
    const char nul_input[] = "GET / HTTP/1.1\r\nHost: local\0host\r\n\r\n";
    assert(http_request_feed(&request, nul_input, sizeof(nul_input) - 1) == HTTP_PARSE_INVALID);
    http_request_destroy(&request);
    ++checks;
    /* 본문은 여기서 해석하지 않는다. 헤더 뒤 큰 데이터도 헤더 한도와 구별한다. */
    char body_input[9000];
    const char *headers = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    memset(body_input, 'a', sizeof(body_input));
    memcpy(body_input, headers, strlen(headers));
    http_request_init(&request);
    assert(http_request_feed(&request, body_input, sizeof(body_input)) == HTTP_PARSE_COMPLETE);
    assert(request.used == strlen(headers));
    http_request_destroy(&request);
    ++checks;

    test_saved_values();
    test_fragments();
    test_query_and_independence();
    test_growth_and_allocation_failure();
    test_size_limit();
    printf("PASS: %zu parser checks\n", checks);
    return 0;
}
