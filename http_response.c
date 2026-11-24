#include "http_response.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <inttypes.h>

/* send가 일부만 보내면 보낸 길이를 누적하고 나머지를 이어 보낸다.
 * 성공은 송신 버퍼에 전달했다는 뜻이며 상대가 읽었다는 뜻은 아니다. */
static int send_all(int client, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(client, data + sent, length - sent, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        sent += (size_t)count;
    }
    return 0;
}

/* 상태 코드에 맞는 짧은 본문을 만들고 상태 줄·헤더 → 본문 순서로 전송한다. */
int http_send_status(int client, int status)
{
    const char *reason;
    switch (status) {
    case 200: reason = "OK"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 400: reason = "Bad Request"; break;
    case 431: reason = "Request Header Fields Too Large"; break;
    case 500: reason = "Internal Server Error"; break;
    case 408: reason = "Request Timeout"; break;
    case 413: reason = "Content Too Large"; break;
    case 417: reason = "Expectation Failed"; break;
    case 501: reason = "Not Implemented"; break;
    case 503: reason = "Service Unavailable"; break;
    case 505: reason = "HTTP Version Not Supported"; break;
    default: errno = EINVAL; return -1;
    }
    const char *body = status == 200 ? "hello world" : reason;
    /* 메모리 부족으로 500을 보낼 때에도 추가 동적 할당은 하지 않는다. */
    char header[256];
    int length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", status, reason, strlen(body));
    if (length < 0 || (size_t)length >= sizeof(header)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (send_all(client, header, (size_t)length) < 0)
        return -1;
    return send_all(client, body, strlen(body));
}

int http_send_hello(int client)
{
    return http_send_status(client, 200);
}


/* 301과 Location을 보내 클라이언트가 새 경로로 요청하도록 한다. */
int http_send_redirect(int client, const char *location)
{
    if (strpbrk(location, "\r\n") || location[0] != '/' || location[1] == '/') {
        errno = EINVAL; return -1;
    }
    const char *prefix = "HTTP/1.1 301 Moved Permanently\r\nLocation: ";
    const char *suffix = "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    if (send_all(client, prefix, strlen(prefix)) < 0 ||
        send_all(client, location, strlen(location)) < 0) return -1;
    return send_all(client, suffix, strlen(suffix));
}

/* 첫 조각 읽기 → 200 헤더 → read/send 반복. 파일 전체를 메모리에 올리지 않는다.
 * 헤더 전 읽기 실패는 500으로 응답한다. 전송 시작 후 실패는 연결 종료로 처리한다.
 * 파일 FD는 호출한 쪽이 닫는다. */
int http_send_file(int client, int fd, off_t size, const char *mime)
{
    char buffer[8192], header[256];
    if (size < 0) return http_send_status(client, 500);
    size_t want = size > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)size;
    ssize_t count;
    do { count = read(fd, buffer, want); } while (count < 0 && errno == EINTR);
    if (count < 0 || (count == 0 && size > 0)) return http_send_status(client, 500);
    int length = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %jd\r\nConnection: close\r\n\r\n",
        mime, (intmax_t)size);
    if (length < 0 || (size_t)length >= sizeof(header)) return http_send_status(client, 500);
    if (send_all(client, header, (size_t)length) < 0) return -1;
    off_t remaining = size;
    while (remaining > 0) {
        if (count <= 0) return -1; /* 응답을 시작했으므로 새 500 응답을 붙이지 않는다. */
        if (send_all(client, buffer, (size_t)count) < 0) return -1;
        remaining -= count;
        if (!remaining) break;
        want = remaining > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        do { count = read(fd, buffer, want); } while (count < 0 && errno == EINTR);
    }
    return 0;
}

/* 이벤트 루프용: 전송하지 않고 헤더만 만든다. */
int http_format_header(char *out, size_t capacity, int status, const char *mime,
                       off_t length, const char *location)
{
    const char *reason = http_status_reason(status);
    if (!reason) return -1;
    int n;
    if (location)
        n = snprintf(out, capacity, "HTTP/1.1 %d %s\r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, reason, location);
    else
        n = snprintf(out, capacity, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %jd\r\nConnection: close\r\n\r\n", status, reason, mime, (intmax_t)length);
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}
const char *http_status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return NULL;
    }
}

/* 204에는 본문뿐 아니라 Content-Length도 넣지 않는다. */
int http_format_no_content(char *out, size_t capacity)
{
    int n = snprintf(out, capacity, "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}
