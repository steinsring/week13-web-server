#include "http_request.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* locale 설정과 무관하게 HTTP에서 사용하는 ASCII 문자만 검사한다. */
static int ascii_alnum(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

static int token_char(unsigned char c)
{
    return ascii_alnum(c) || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int valid_token(const char *text)
{
    if (*text == '\0')
        return 0;
    for (; *text; ++text) {
        if (!token_char((unsigned char)*text))
            return 0;
    }
    return 1;
}

static unsigned char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int same_name(const char *a, const char *b)
{
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) != ascii_lower((unsigned char)*b))
            return 0;
        ++a;
        ++b;
    }
    return *a == *b;
}

void http_request_init(HttpRequest *request)
{
    memset(request, 0, sizeof(*request));
}

void http_request_destroy(HttpRequest *request)
{
    free(request->headers);
    /* 이전 요청의 데이터와 포인터를 다음 요청에 남기지 않는다. */
    memset(request, 0, sizeof(*request));
}

const HttpHeader *http_request_next_header(const HttpRequest *request,
                                         const char *name, size_t *cursor)
{
    while (*cursor < request->header_count) {
        const HttpHeader *header = &request->headers[(*cursor)++];
        if (same_name(header->name, name))
            return header;
    }
    return NULL;
}

/* 요청 첫 줄: 메서드 SP 요청대상 SP HTTP/1.1 */
static HttpParseResult parse_request_line(HttpRequest *request, char *line)
{
    char *first_space = strchr(line, ' ');
    if (!first_space)
        return HTTP_PARSE_INVALID;
    char *target = first_space + 1;
    char *second_space = strchr(target, ' ');
    if (!second_space)
        return HTTP_PARSE_INVALID;
    char *version = second_space + 1;
    *first_space = '\0';
    *second_space = '\0';

    if (!valid_token(line) || *target == '\0' || strlen(version) != 8 ||
        memcmp(version, "HTTP/", 5) != 0 || version[5] < '0' || version[5] > '9' ||
        version[6] != '.' || version[7] < '0' || version[7] > '9')
        return HTTP_PARSE_INVALID;
    /* 공백·탭·제어 문자는 거절한다. %20 같은 표기는 원문을 유지한다. */
    for (const unsigned char *p = (const unsigned char *)target; *p; ++p) {
        if (*p <= 0x20 || *p == 0x7f)
            return HTTP_PARSE_INVALID;
    }
    if (strcmp(version, "HTTP/1.1") != 0)
        return HTTP_PARSE_UNSUPPORTED_VERSION;
    request->method = line;
    request->version = version;
    request->path = target;
    char *question = strchr(target, '?');
    if (question) {
        *question = '\0';
        request->query = question + 1;
    }
    request->request_line_done = 1;
    return HTTP_PARSE_INCOMPLETE;
}

static HttpParseResult append_header(HttpRequest *request, char *name, char *value)
{
    if (request->header_count == request->header_capacity) {
        /* 문자열 버퍼는 이동하지 않고, 포인터 목록만 늘린다. */
        size_t capacity = request->header_capacity ? request->header_capacity * 2 : 8;
        if (capacity < request->header_capacity || capacity > SIZE_MAX / sizeof(HttpHeader))
            return HTTP_PARSE_NO_MEMORY;
        HttpHeader *grown = realloc(request->headers, capacity * sizeof(*grown));
        if (!grown)
            return HTTP_PARSE_NO_MEMORY;
        request->headers = grown;
        request->header_capacity = capacity;
    }
    request->headers[request->header_count++] = (HttpHeader){name, value};
    return HTTP_PARSE_INCOMPLETE;
}

static HttpParseResult parse_header(HttpRequest *request, char *line)
{
    char *colon = strchr(line, ':');
    if (!colon)
        return HTTP_PARSE_INVALID;
    *colon = '\0';
    if (!valid_token(line))
        return HTTP_PARSE_INVALID;

    char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
        ++value;
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    *end = '\0';
    /* 내부 공백과 탭은 유지한다. NUL/CR/LF는 입력을 받을 때 검사한다. */
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if ((*p < 0x20 && *p != '\t') || *p == 0x7f)
            return HTTP_PARSE_INVALID;
    }
    return append_header(request, line, value);
}

static int hex_digit(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* DNS 조회는 하지 않는다. URI의 reg-name 문자와 %XX 표기만 검사한다. */
static int valid_host_name(const char *start, const char *end)
{
    if (start == end)
        return 0;
    for (const char *p = start; p < end; ++p) {
        unsigned char c = (unsigned char)*p;
        if (ascii_alnum(c) || strchr("-._~!$&'()*+,;=", c))
            continue;
        if (c == '%' && end - p >= 3 && hex_digit((unsigned char)p[1]) &&
            hex_digit((unsigned char)p[2])) {
            p += 2;
            continue;
        }
        return 0;
    }
    return 1;
}

static int valid_host(const char *value)
{
    const char *port = NULL;
    if (*value == '[') {
        const char *closing = strchr(value, ']');
        char address[INET6_ADDRSTRLEN];
        struct in6_addr parsed;
        if (!closing || closing - value <= 1 ||
            (size_t)(closing - value - 1) >= sizeof(address))
            return 0;
        size_t length = (size_t)(closing - value - 1);
        memcpy(address, value + 1, length);
        address[length] = '\0';
        if (inet_pton(AF_INET6, address, &parsed) != 1)
            return 0;
        if (closing[1] == ':')
            port = closing + 2;
        else if (closing[1] != '\0')
            return 0;
    } else {
        const char *colon = strchr(value, ':');
        const char *end = colon ? colon : value + strlen(value);
        if (!valid_host_name(value, end))
            return 0;
        if (colon)
            port = colon + 1;
    }
    if (port) {
        if (*port == '\0')
            return 0;
        for (; *port; ++port) {
            if (*port < '0' || *port > '9')
                return 0;
        }
    }
    return 1;
}

static HttpParseResult finish_headers(const HttpRequest *request)
{
    size_t cursor = 0;
    const HttpHeader *host = http_request_next_header(request, "Host", &cursor);
    if (!host || !valid_host(host->value) ||
        http_request_next_header(request, "Host", &cursor))
        return HTTP_PARSE_INVALID;
    return HTTP_PARSE_COMPLETE;
}

HttpParseResult http_request_feed(HttpRequest *request, const char *data, size_t length)
{
    if (request->result != HTTP_PARSE_INCOMPLETE)
        return request->result;

    for (size_t i = 0; i < length; ++i) {
        if (request->used == HTTP_HEADER_LIMIT)
            return request->result = HTTP_PARSE_TOO_LARGE;
        char c = data[i];
        /* C 문자열을 일찍 끝내는 NUL과 CRLF가 아닌 줄바꿈은 거절한다. */
        if (c == '\0' || (request->used > request->line_start &&
            request->buffer[request->used - 1] == '\r' && c != '\n'))
            return request->result = HTTP_PARSE_INVALID;
        request->buffer[request->used++] = c;
        request->buffer[request->used] = '\0';
        if (c == '\n') {
            if (request->used - request->line_start < 2 ||
                request->buffer[request->used - 2] != '\r')
                return request->result = HTTP_PARSE_INVALID;

            char *line = request->buffer + request->line_start;
            request->buffer[request->used - 2] = '\0';
            request->buffer[request->used - 1] = '\0';
            request->line_start = request->used;
            if (!request->request_line_done)
                request->result = parse_request_line(request, line);
            else if (*line == '\0')
                request->result = finish_headers(request);
            else
                request->result = parse_header(request, line);
            if (request->result != HTTP_PARSE_INCOMPLETE)
                return request->result;
        }
        if (request->used == HTTP_HEADER_LIMIT)
            return request->result = HTTP_PARSE_TOO_LARGE;
    }
    return request->result;
}
