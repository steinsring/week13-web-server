#ifndef REQUEST_IO_H
#define REQUEST_IO_H

#include "http_request.h"
#include <time.h>

/* 성공은 200, 오류는 HTTP 상태 코드, 응답 불가능한 단절은 0.
 * accepted는 accept 성공 직후의 CLOCK_MONOTONIC 시각이다.
 */
int http_receive_request(int client, HttpRequest *request, const struct timespec *accepted);

#endif
