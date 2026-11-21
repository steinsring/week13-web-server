#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H
#include <sys/types.h>

/* client 소켓에 고정 HTTP 응답 전체를 보낸다. 성공 0, 실패 -1. */
int http_send_hello(int client);
int http_send_status(int client, int status);

int http_send_redirect(int client, const char *location);
int http_send_file(int client, int fd, off_t size, const char *mime);
#endif
