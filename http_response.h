#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

/* client 소켓에 고정 HTTP 응답 전체를 보낸다. 성공 0, 실패 -1. */
int http_send_hello(int client);

#endif
