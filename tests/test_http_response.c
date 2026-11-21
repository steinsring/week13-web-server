#define _POSIX_C_SOURCE 200809L
/* 테스트 빌드에서만 send를 감싸 부분 송신과 EINTR를 재현한다. */
#ifdef send
#undef send
#endif
#include "http_response.h"
#include "request_io.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int interrupted;
static int broken;
ssize_t test_send(int fd, const void *data, size_t length, int flags)
{
    if (broken) { errno = EPIPE; return -1; }
    if (interrupted) { interrupted = 0; errno = EINTR; return -1; }
    return send(fd, data, length > 3 ? 3 : length, flags);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const int codes[] = {200, 400, 431, 500, 408, 501, 505};
    const char *reasons[] = {"OK", "Bad Request", "Request Header Fields Too Large",
        "Internal Server Error", "Request Timeout", "Not Implemented", "HTTP Version Not Supported"};
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        int pair[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        interrupted = 1;
        assert(http_send_status(pair[0], codes[i]) == 0);
        close(pair[0]);
        char actual[512], expected[512];
        size_t used = 0;
        ssize_t count;
        while ((count = read(pair[1], actual + used, sizeof(actual) - used)) > 0)
            used += (size_t)count;
        assert(count == 0);
        const char *body = codes[i] == 200 ? "hello world" : reasons[i];
        int length = snprintf(expected, sizeof(expected),
            "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
            codes[i], reasons[i], strlen(body), body);
        assert(used == (size_t)length && memcmp(actual, expected, used) == 0);
        close(pair[1]);
    }
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    broken = 1;
    assert(http_send_status(pair[0], 400) == -1);
    broken = 0;
    HttpRequest request;
    http_request_init(&request);
    struct timespec accepted;
    assert(clock_gettime(CLOCK_MONOTONIC, &accepted) == 0);
    accepted.tv_sec -= 11;
    int flags = fcntl(pair[0], F_GETFL);
    assert(http_receive_request(pair[0], &request, &accepted) == 408);
    assert(fcntl(pair[0], F_GETFL) == flags);
    http_request_destroy(&request);
    close(pair[0]); close(pair[1]);
    /* 첫 읽기 실패는 500, 200 헤더 뒤 조기 EOF는 새 오류 응답 없이 실패한다. */
    for (int early_eof = 0; early_eof <= 1; ++early_eof) {
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        char name[] = "/tmp/static-response-XXXXXX";
        int file = mkstemp(name);
        assert(file >= 0);
        unlink(name);
        assert(write(file, "data", 4) == 4);
        assert(lseek(file, 0, SEEK_SET) == 0);
        interrupted = 1;
        int result = http_send_file(pair[0], early_eof ? file : -1,
                                    early_eof ? 8 : 1, "text/plain");
        assert(result == (early_eof ? -1 : 0));
        close(file);
        close(pair[0]);
        char actual[512];
        size_t used = 0;
        ssize_t count;
        while ((count = read(pair[1], actual + used, sizeof(actual) - used - 1)) > 0)
            used += (size_t)count;
        assert(count == 0);
        actual[used] = '\0';
        if (early_eof) {
            assert(strncmp(actual, "HTTP/1.1 200 OK", 15) == 0);
            assert(strstr(actual, "Content-Length: 8\r\n"));
            assert(strstr(actual, "\r\n\r\ndata"));
            assert(!strstr(actual, "500"));
        } else {
            assert(strncmp(actual, "HTTP/1.1 500 Internal Server Error", 34) == 0);
            assert(strstr(actual, "\r\n\r\nInternal Server Error"));
        }
        close(pair[1]);
    }
    puts("PASS: response formats, partial send, EINTR, send failure, deadline, file read failure and early EOF");
    return 0;
}
