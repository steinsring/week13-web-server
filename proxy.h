#ifndef PROXY_H
#define PROXY_H
#include "http_body.h"
#include "http_request.h"
#include <stdint.h>
#include <sys/socket.h>

#define PROXY_BUFFER 8192
typedef struct {
    char text[512], host[256], authority[320];
    unsigned port;
    int https;
} ProxyOrigin;
typedef struct {
    ProxyOrigin origin;
    struct sockaddr_storage address;
    socklen_t address_length;
} ProxyBackend;
typedef struct {
    ProxyOrigin public_origin;
    ProxyBackend* backends;
    size_t count, next;
    int64_t connect_ms, idle_ms;
} ProxyConfig;
typedef struct Proxy Proxy;

int proxy_config_init(ProxyConfig* config, const char* public_url, const char* backends,
    size_t connect_seconds, size_t idle_seconds);
void proxy_config_destroy(ProxyConfig* config);
int proxy_route(const char* path);
int proxy_rewrite_location(const ProxyConfig* config, const char* value, char* out, size_t capacity);
Proxy* proxy_start(ProxyConfig* config, const HttpRequest* request, const HttpBody* body,
    int close_frontend, int64_t now, int* status);
void proxy_destroy(Proxy* proxy);
int proxy_socket(const Proxy* proxy);
uint32_t proxy_interest(const Proxy* proxy);
int64_t proxy_deadline(const Proxy* proxy);
/* 양수 반환은 최종 HTTP 오류, 0은 정상 진행이다. socket I/O는 호출 이벤트 루프에서만 실행한다. */
int proxy_step(Proxy* proxy, uint32_t events, int64_t now);
const char* proxy_output(const Proxy* proxy, size_t* length, int* final);
void proxy_consume_output(Proxy* proxy, size_t length, int64_t now);
int proxy_finished(const Proxy* proxy);
int proxy_frontend_close(const Proxy* proxy);
int proxy_mark_close(Proxy* proxy);
size_t proxy_connection_size(void);
#endif
