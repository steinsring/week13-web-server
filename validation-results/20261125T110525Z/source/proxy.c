#define _GNU_SOURCE
#include "proxy.h"
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <unistd.h>

#define OUTPUT_CAP (PROXY_BUFFER * 2)
typedef enum { CONNECTING,
    REQUEST_SENDING,
    RESPONSE_READING,
    FINISHED } UpstreamPhase;
typedef enum { HEADERS,
    LENGTH_BODY,
    EOF_BODY,
    CHUNK_SIZE,
    CHUNK_DATA,
    CHUNK_CRLF,
    TRAILERS } ResponsePhase;
typedef struct {
    char *name, *value;
} ResponseHeader;
struct Proxy {
    ProxyConfig* config;
    int fd, frontend_close, suppress_100;
    UpstreamPhase phase;
    ResponsePhase response_phase;
    int64_t deadline, paused_remaining;
    int paused;
    char request[OUTPUT_CAP];
    size_t request_length, request_sent;
    const unsigned char* request_body;
    size_t body_length, body_sent;
    char input[PROXY_BUFFER];
    size_t input_length, input_position;
    char header[PROXY_BUFFER + 1];
    size_t header_length, interim_total;
    ResponseHeader headers[PROXY_BUFFER / 4];
    size_t header_count;
    int status, has_length, chunked, encode_chunks;
    uint64_t remaining;
    char line[PROXY_BUFFER + 1];
    size_t line_length, trailer_length, crlf;
    char output[OUTPUT_CAP];
    size_t output_length, output_sent;
    int output_final, output_header;
};

static int token(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c && strchr("!#$%&'*+-.^_`|~", c));
}
static int append(char* out, size_t capacity, size_t* used, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out + *used, capacity - *used, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= capacity - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}
static int named_token(const char* list, const char* name)
{
    while (*list) {
        while (*list == ' ' || *list == '\t' || *list == ',')
            ++list;
        const char* start = list;
        while (*list && *list != ',')
            ++list;
        const char* end = list;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            --end;
        if ((size_t)(end - start) == strlen(name) && !strncasecmp(start, name, (size_t)(end - start)))
            return 1;
    }
    return 0;
}
static int hop_header(const char* name)
{
    static const char* names[] = { "Connection", "Keep-Alive", "Proxy-Connection", "TE", "Trailer",
        "Transfer-Encoding", "Upgrade", "Proxy-Authenticate", "Proxy-Authorization" };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
        if (!strcasecmp(name, names[i]))
            return 1;
    return 0;
}
static int request_hop(const HttpRequest* request, const char* name)
{
    if (hop_header(name) || !strcasecmp(name, "Host") || !strcasecmp(name, "Content-Length") || !strcasecmp(name, "Expect") || !strcasecmp(name, "Forwarded") || !strncasecmp(name, "X-Forwarded", 11))
        return 1;
    size_t cursor = 0;
    const HttpHeader* header;
    while ((header = http_request_next_header(request, "Connection", &cursor)))
        if (named_token(header->value, name))
            return 1;
    return 0;
}

/* 클라이언트 구간의 연결·신뢰 헤더는 제거하고 이번 upstream 구간의 경계를 생성한다. */
static int build_request(Proxy* p, const ProxyBackend* backend, const HttpRequest* request)
{
    if (append(p->request, sizeof(p->request), &p->request_length, "%s %s%s%s HTTP/1.1\r\nHost: %s\r\n",
            request->method, request->path, request->query ? "?" : "", request->query ? request->query : "", backend->origin.authority))
        return -1;
    for (size_t i = 0; i < request->header_count; ++i) {
        const HttpHeader* h = &request->headers[i];
        if (!request_hop(request, h->name) && append(p->request, sizeof(p->request), &p->request_length, "%s: %s\r\n", h->name, h->value))
            return -1;
    }
    return append(p->request, sizeof(p->request), &p->request_length,
        "Content-Length: %zu\r\nConnection: close\r\nVia: 1.1 ljm-proxy\r\n\r\n", p->body_length);
}
static void close_upstream(Proxy* p)
{
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
}
void proxy_destroy(Proxy* p)
{
    if (!p)
        return;
    close_upstream(p);
    free(p);
}
Proxy* proxy_start(ProxyConfig* config, const HttpRequest* request, const HttpBody* body,
    int close_frontend, int64_t now, int* status)
{
    *status = 500;
    Proxy* p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->fd = -1;
    p->config = config;
    p->frontend_close = close_frontend;
    p->request_body = body->bytes;
    p->body_length = body->length;
    size_t cursor = 0;
    p->suppress_100 = http_request_next_header(request, "Expect", &cursor) != NULL;
    ProxyBackend* backend = &config->backends[config->next];
    config->next = (config->next + 1) % config->count;
    if (build_request(p, backend, request))
        goto fail;
    *status = 502;
    p->fd = socket(backend->address.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (p->fd < 0)
        goto fail;
    int result = connect(p->fd, (struct sockaddr*)&backend->address, backend->address_length);
    if (result < 0 && errno != EINPROGRESS)
        goto fail;
    p->phase = result == 0 ? REQUEST_SENDING : CONNECTING;
    p->deadline = now + (result == 0 ? config->idle_ms : config->connect_ms);
    *status = 0;
    return p;
fail:
    proxy_destroy(p);
    return NULL;
}

static int split_header(char* line, ResponseHeader* header)
{
    char* colon = strchr(line, ':');
    if (!colon || colon == line)
        return -1;
    for (char* q = line; q < colon; ++q)
        if (!token((unsigned char)*q))
            return -1;
    *colon = 0;
    char* value = colon + 1;
    while (*value == ' ' || *value == '\t')
        ++value;
    for (const unsigned char* q = (unsigned char*)value; *q; ++q)
        if ((*q < 32 && *q != '\t') || *q == 127)
            return -1;
    char* end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
        --end;
    *end = 0;
    header->name = line;
    header->value = value;
    return 0;
}
static int decimal_length(const char* text, uint64_t* value)
{
    if (!*text)
        return -1;
    uint64_t n = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || n > (UINT64_MAX - (unsigned)(*text - '0')) / 10)
            return -1;
        n = n * 10 + (unsigned)(*text - '0');
    }
    *value = n;
    return 0;
}
static int response_hop(Proxy* p, const char* name)
{
    if (hop_header(name) || !strcasecmp(name, "Content-Length"))
        return 1;
    for (size_t i = 0; i < p->header_count; ++i)
        if (!strcasecmp(p->headers[i].name, "Connection") && named_token(p->headers[i].value, name))
            return 1;
    return 0;
}
static int reusable_status(int status)
{
    return status == 200 || status == 204 || status == 301 || status == 403 || status == 404;
}

static int serialize_response(Proxy* p)
{
    size_t used = 0;
    if (append(p->output, sizeof(p->output), &used, "HTTP/1.1 %d%s\r\n", p->status, p->header + 12))
        return -1;
    for (size_t i = 0; i < p->header_count; ++i) {
        ResponseHeader* h = &p->headers[i];
        if (response_hop(p, h->name))
            continue;
        char rewritten[PROXY_BUFFER + 512];
        const char* value = h->value;
        if (!strcasecmp(h->name, "Location")) {
            if (proxy_rewrite_location(p->config, value, rewritten, sizeof(rewritten)))
                return -1;
            value = rewritten;
        }
        if (append(p->output, sizeof(p->output), &used, "%s: %s\r\n", h->name, value))
            return -1;
    }
    if (p->status >= 200 && p->status != 204) {
        if (p->has_length) {
            if (append(p->output, sizeof(p->output), &used, "Content-Length: %" PRIu64 "\r\n", p->remaining))
                return -1;
        } else if (p->status != 304 && append(p->output, sizeof(p->output), &used, "Transfer-Encoding: chunked\r\n"))
            return -1;
    }
    if (p->status >= 200 && p->frontend_close && append(p->output, sizeof(p->output), &used, "Connection: close\r\n"))
        return -1;
    if (append(p->output, sizeof(p->output), &used, "Via: 1.1 ljm-proxy\r\n\r\n"))
        return -1;
    p->output_length = used;
    p->output_sent = 0;
    p->output_final = p->status >= 200;
    p->output_header = 1;
    return 0;
}

/* 헤더를 먼저 전부 검증한다. Connection으로 CL/TE를 숨겨 경계 검사를 우회할 수 없다. */
static int parse_response_headers(Proxy* p)
{
    char *line = p->header, *end = strstr(line, "\r\n");
    if (!end)
        return 502;
    *end = 0;
    if (strlen(line) < 13 || (strncmp(line, "HTTP/1.1 ", 9) && strncmp(line, "HTTP/1.0 ", 9)) || line[12] != ' ')
        return 502;
    if (line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' || line[11] < '0' || line[11] > '9')
        return 502;
    for (unsigned char* q = (unsigned char*)line + 13; *q; ++q)
        if ((*q < 32 && *q != '\t') || *q == 127)
            return 502;
    p->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
    if (p->status == 101)
        return 502;
    p->header_count = 0;
    p->has_length = p->chunked = 0;
    int locations = 0;
    line = end + 2;
    while (*line) {
        end = strstr(line, "\r\n");
        if (!end)
            return 502;
        *end = 0;
        if (!*line)
            break;
        if (p->header_count == sizeof(p->headers) / sizeof(*p->headers))
            return 502;
        ResponseHeader* h = &p->headers[p->header_count++];
        if (split_header(line, h))
            return 502;
        if (!strcasecmp(h->name, "Content-Length")) {
            if (p->has_length || decimal_length(h->value, &p->remaining))
                return 502;
            p->has_length = 1;
        }
        if (!strcasecmp(h->name, "Transfer-Encoding")) {
            if (p->chunked || strcasecmp(h->value, "chunked"))
                return 502;
            p->chunked = 1;
        }
        if (!strcasecmp(h->name, "Location")) {
            char rewritten[PROXY_BUFFER + 512];
            if (++locations > 1 || proxy_rewrite_location(p->config, h->value, rewritten, sizeof(rewritten)))
                return 502;
        }
        line = end + 2;
    }
    if (p->has_length && p->chunked)
        return 502;
    if ((p->status < 200 || p->status == 204) && (p->has_length || p->chunked))
        return 502;
    if (p->status < 200) {
        p->interim_total += p->header_length;
        if (p->interim_total > PROXY_BUFFER)
            return 502;
        if (p->status != 100 || !p->suppress_100) {
            if (serialize_response(p))
                return 502;
        }
        p->response_phase = HEADERS;
        p->header_length = 0;
        return 0;
    }
    if (!reusable_status(p->status))
        p->frontend_close = 1;
    p->encode_chunks = !p->has_length && p->status != 204 && p->status != 304;
    if (serialize_response(p))
        return 502;
    if (p->status == 204 || p->status == 304 || (p->has_length && !p->remaining)) {
        p->phase = FINISHED;
        close_upstream(p);
    } else
        p->response_phase = p->has_length ? LENGTH_BODY : (p->chunked ? CHUNK_SIZE : EOF_BODY);
    return 0;
}

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
static void space(const char** p)
{
    while (**p == ' ' || **p == '\t')
        ++*p;
}
static int chunk_extension(const char* p)
{
    while (*p) {
        space(&p);
        if (*p++ != ';')
            return -1;
        space(&p);
        const char* start = p;
        while (token((unsigned char)*p))
            ++p;
        if (p == start)
            return -1;
        space(&p);
        if (*p == '=') {
            ++p;
            space(&p);
            if (*p == '"') {
                ++p;
                while (*p && *p != '"') {
                    unsigned char c = (unsigned char)*p++;
                    if (c == '\\') {
                        c = (unsigned char)*p;
                        if (!c)
                            return -1;
                        ++p;
                    }
                    if ((c < 32 && c != '\t') || c == 127)
                        return -1;
                }
                if (*p++ != '"')
                    return -1;
            } else {
                start = p;
                while (token((unsigned char)*p))
                    ++p;
                if (p == start)
                    return -1;
            }
            space(&p);
        }
    }
    return 0;
}
static int parse_chunk_size(Proxy* p)
{
    const char* q = p->line;
    int digit = hex_digit((unsigned char)*q);
    if (digit < 0)
        return 502;
    uint64_t size = 0;
    while ((digit = hex_digit((unsigned char)*q)) >= 0) {
        if (size > (UINT64_MAX - (unsigned)digit) / 16)
            return 502;
        size = size * 16 + (unsigned)digit;
        ++q;
    }
    if (chunk_extension(q))
        return 502;
    p->remaining = size;
    p->response_phase = size ? CHUNK_DATA : TRAILERS;
    return 0;
}
static void finish_stream(Proxy* p)
{
    p->phase = FINISHED;
    close_upstream(p);
    if (p->encode_chunks) {
        memcpy(p->output, "0\r\n\r\n", 5);
        p->output_length = 5;
        p->output_sent = 0;
        p->output_final = 1;
        p->output_header = 0;
    }
}
static void output_body(Proxy* p, const char* data, size_t length)
{
    size_t start = 0;
    if (p->encode_chunks)
        start = (size_t)snprintf(p->output, sizeof(p->output), "%zx\r\n", length);
    memcpy(p->output + start, data, length);
    start += length;
    if (p->encode_chunks) {
        memcpy(p->output + start, "\r\n", 2);
        start += 2;
    }
    p->output_length = start;
    p->output_sent = 0;
    p->output_final = 1;
    p->output_header = 0;
}

/* 응답 전체 대신 한 조각만 만든다. 출력이 남으면 파싱·추가 수신을 멈춘다. */
static int pump_input(Proxy* p)
{
    while (p->input_position < p->input_length && !p->output_length && p->phase == RESPONSE_READING) {
        if (p->response_phase == HEADERS) {
            char c = p->input[p->input_position++];
            if (!c || p->header_length == PROXY_BUFFER)
                return 502;
            p->header[p->header_length++] = c;
            p->header[p->header_length] = 0;
            if (p->header_length >= 4 && !memcmp(p->header + p->header_length - 4, "\r\n\r\n", 4)) {
                int status = parse_response_headers(p);
                if (status)
                    return status;
            }
        } else if (p->response_phase == LENGTH_BODY || p->response_phase == EOF_BODY || p->response_phase == CHUNK_DATA) {
            size_t n = p->input_length - p->input_position;
            if (p->response_phase != EOF_BODY && p->remaining < n)
                n = (size_t)p->remaining;
            if (!n)
                return 502;
            output_body(p, p->input + p->input_position, n);
            p->input_position += n;
            if (p->response_phase != EOF_BODY) {
                p->remaining -= n;
                if (!p->remaining) {
                    if (p->response_phase == LENGTH_BODY) {
                        p->phase = FINISHED;
                        close_upstream(p);
                    } else {
                        p->response_phase = CHUNK_CRLF;
                        p->crlf = 0;
                    }
                }
            }
        } else if (p->response_phase == CHUNK_CRLF) {
            const char* expected = "\r\n";
            if (p->input[p->input_position++] != expected[p->crlf++])
                return 502;
            if (p->crlf == 2)
                p->response_phase = CHUNK_SIZE;
        } else {
            char c = p->input[p->input_position++];
            if (!c || p->line_length == PROXY_BUFFER)
                return 502;
            if (p->response_phase == TRAILERS && ++p->trailer_length > PROXY_BUFFER)
                return 502;
            p->line[p->line_length++] = c;
            p->line[p->line_length] = 0;
            if (c == '\n') {
                if (p->line_length < 2 || p->line[p->line_length - 2] != '\r')
                    return 502;
                p->line[p->line_length - 2] = 0;
                if (p->response_phase == CHUNK_SIZE) {
                    int status = parse_chunk_size(p);
                    if (status)
                        return status;
                } else if (p->line_length == 2)
                    finish_stream(p);
                else {
                    ResponseHeader h;
                    if (split_header(p->line, &h) || hop_header(h.name) || !strcasecmp(h.name, "Content-Length") || !strcasecmp(h.name, "Host"))
                        return 502;
                }
                p->line_length = 0;
            }
        }
    }
    return 0;
}
static void pause_reading(Proxy* p, int64_t now)
{
    if (p->phase == RESPONSE_READING && p->output_length && !p->paused) {
        p->paused_remaining = p->deadline > now ? p->deadline - now : 0;
        p->paused = 1;
    }
}
static int send_request(Proxy* p, int64_t now)
{
    for (unsigned turn = 0; turn < 16 && (p->request_sent < p->request_length || p->body_sent < p->body_length); ++turn) {
        int header = p->request_sent < p->request_length;
        const void* data = header ? (const void*)(p->request + p->request_sent) : (const void*)(p->request_body + p->body_sent);
        size_t nbytes = header ? p->request_length - p->request_sent : p->body_length - p->body_sent;
        if (nbytes > PROXY_BUFFER)
            nbytes = PROXY_BUFFER;
        ssize_t n = send(p->fd, data, nbytes, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n <= 0)
            return 502;
        if (header)
            p->request_sent += (size_t)n;
        else
            p->body_sent += (size_t)n;
        p->deadline = now + p->config->idle_ms;
    }
    if (p->request_sent < p->request_length || p->body_sent < p->body_length)
        return 0;
    p->phase = RESPONSE_READING;
    p->deadline = now + p->config->idle_ms;
    return 0;
}
int proxy_step(Proxy* p, uint32_t events, int64_t now)
{
    if (p->phase == FINISHED)
        return 0;
    if (!p->paused && now >= p->deadline)
        return 504;
    if (p->phase == CONNECTING) {
        if (!(events & (EPOLLOUT | EPOLLERR | EPOLLHUP)))
            return 0;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
            return 502;
        p->phase = REQUEST_SENDING;
        p->deadline = now + p->config->idle_ms;
    }
    if (p->phase == REQUEST_SENDING) {
        if (events & EPOLLERR)
            return 502;
        int status = send_request(p, now);
        if (status)
            return status;
        if (p->phase != RESPONSE_READING)
            return 0;
    }
    if (p->output_length)
        return 0;
    int status = pump_input(p);
    if (status)
        return status;
    if (p->output_length || p->phase == FINISHED) {
        pause_reading(p, now);
        return 0;
    }
    if (!(events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)))
        return 0;
    /* 한 이벤트에서는 한 조각만 수신한다. LT epoll이 남은 데이터를 다시 알려준다. */
    ssize_t n = recv(p->fd, p->input, sizeof(p->input), 0);
    if (n < 0 && errno == EINTR)
        return 0;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    if (n < 0)
        return 502;
    if (n == 0) {
        if (p->response_phase != EOF_BODY)
            return 502;
        finish_stream(p);
        return 0;
    }
    p->deadline = now + p->config->idle_ms;
    p->input_length = (size_t)n;
    p->input_position = 0;
    status = pump_input(p);
    pause_reading(p, now);
    return status;
}
int proxy_socket(const Proxy* p) { return p->fd; }
uint32_t proxy_interest(const Proxy* p)
{
    if (p->phase == CONNECTING || p->phase == REQUEST_SENDING)
        return EPOLLOUT;
    if (p->phase == RESPONSE_READING && !p->output_length)
        return EPOLLIN | EPOLLRDHUP;
    return 0;
}
int64_t proxy_deadline(const Proxy* p) { return p->paused || p->phase == FINISHED ? INT64_MAX : p->deadline; }
const char* proxy_output(const Proxy* p, size_t* length, int* final)
{
    *length = p->output_length - p->output_sent;
    *final = p->output_final;
    return p->output + p->output_sent;
}
void proxy_consume_output(Proxy* p, size_t length, int64_t now)
{
    p->output_sent += length;
    if (p->output_sent == p->output_length) {
        p->output_length = p->output_sent = 0;
        if (p->paused) {
            p->paused = 0;
            p->deadline = now + p->paused_remaining;
        }
    }
}
int proxy_finished(const Proxy* p) { return p->phase == FINISHED && !p->output_length; }
int proxy_frontend_close(const Proxy* p) { return p->frontend_close; }
int proxy_mark_close(Proxy* p)
{
    p->frontend_close = 1;
    if (p->output_length && p->output_header && p->output_final && !p->output_sent)
        return serialize_response(p);
    return 0;
}
size_t proxy_connection_size(void) { return sizeof(Proxy); }
