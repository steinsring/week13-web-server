#define _POSIX_C_SOURCE 200809L
#include "request_io.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>

/* 연결 수락 시각으로부터 남은 총시간. 데이터가 와도 기한을 갱신하지 않는다. */
static int remaining_ms(const struct timespec *accepted)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    double milliseconds = (10.0 + (double)(accepted->tv_sec - now.tv_sec)) * 1000.0 +
                          (double)(accepted->tv_nsec - now.tv_nsec) / 1000000.0;
    if (milliseconds <= 0)
        return 0;
    int rounded = (int)milliseconds;
    return rounded + (milliseconds > rounded); /* poll의 밀리초 단위로 올림 */
}


/* 남은 시간 계산 → poll 대기 → recv 수신 → 파서 전달을 반복한다.
 * 완료·오류 시 HTTP 상태 코드를 반환한다. 0이면 응답 없이 연결을 정리한다.
 * recv 한 번이 요청 전체와 일치하지 않으므로 미완성 요청은 이어서 받는다. */
static int receive_until_deadline(int client, HttpRequest *request,
                                  const struct timespec *accepted)
{
    char chunk[4096];
    for (;;) {
        int remaining = remaining_ms(accepted);
        if (remaining < 0)
            return 500;
        if (remaining == 0)
            return 408;

        struct pollfd fd = {client, POLLIN, 0};
        int ready = poll(&fd, 1, remaining);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (ready == 0)
            continue; /* 단조 시계로 실제 기한을 다시 확인 */
        if (fd.revents & (POLLNVAL | POLLERR))
            return 0;

        ssize_t count = recv(client, chunk, sizeof(chunk), 0);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return 0;
        }
        if (count == 0)
            return request->used ? 400 : 0;
        /* 기한을 넘겨 읽은 조각으로 요청이 완성되어도 시간 초과다. */
        remaining = remaining_ms(accepted);
        if (remaining < 0)
            return 500;
        if (remaining == 0)
            return 408;

        switch (http_request_feed(request, chunk, (size_t)count)) {
        case HTTP_PARSE_INCOMPLETE: break;
        case HTTP_PARSE_COMPLETE: return strcmp(request->method, "GET") == 0 ? 200 : 501;
        case HTTP_PARSE_INVALID: return 400;
        case HTTP_PARSE_TOO_LARGE: return 431;
        case HTTP_PARSE_NO_MEMORY: return 500;
        case HTTP_PARSE_UNSUPPORTED_VERSION: return 505;
        }
    }
}


/* 수신 중에만 논블로킹을 켠다. poll 이후 recv가 무기한 잠드는 것을 막는다.
 * 수신이 끝나면 원래 설정을 복원한다. 현재 연결 하나만 감시하며 동시 처리는 아니다. */
int http_receive_request(int client, HttpRequest *request, const struct timespec *accepted)
{
    int flags = fcntl(client, F_GETFL);
    if (flags < 0 || fcntl(client, F_SETFL, flags | O_NONBLOCK) < 0)
        return 500;
    /* 준비 알림 직후에도 recv가 무기한 잠들지 않게 한다.
     * 현재 연결 하나만 감시하며, 동시 요청 처리를 추가하는 것은 아니다.
     */
    int status = receive_until_deadline(client, request, accepted);
    if (fcntl(client, F_SETFL, flags) < 0)
        return 0;
    return status; /* 응답은 기존 블로킹 송신 방식으로 보낸다. */
}
