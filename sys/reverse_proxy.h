#ifndef REVERSE_PROXY_H
#define REVERSE_PROXY_H

#include <stddef.h>

typedef struct
{
    char method[16];
    char uri[2048];
    char version[16];
    char host[256];
    size_t content_length;
    const char *body;
    size_t body_received;
    const char *raw_headers;
    size_t headers_length;
} HttpRequest;

int send_all(int fd, const char *buf, size_t len);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
void reverse_proxy(int fd, HttpRequest *request, const char *backend_host, const char *backend_port);

#endif
