#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "server.h"
#include "client.h"

#define MAX_HEADERS 32768
#define IO_SIZE 8192
#define MAX_URL 8192

static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void error_response(int fd, const char *status)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
    if (n > 0 && (size_t)n < sizeof(buf)) send_all(fd, buf, (size_t)n);
}

/* Parse an HTTP absolute URI. The host passed to getaddrinfo excludes IPv6 brackets. */
static int parse_url(const char *url, char *host, size_t host_size,
                     char *port, size_t port_size, char *path, size_t path_size)
{
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *start = url + 7;
    const char *end = start + strcspn(start, "/?#");
    if (end == start || memchr(start, '@', (size_t)(end - start)) || *end == '#') return -1;
    const char *host_start = start, *host_end = end, *port_start = NULL;
    if (*start == '[') {
        host_end = memchr(start + 1, ']', (size_t)(end - start - 1));
        if (!host_end || host_end == start + 1) return -1;
        host_start = start + 1;
        if (host_end + 1 < end) {
            if (host_end[1] != ':') return -1;
            port_start = host_end + 2;
        }
    } else {
        const char *colon = memchr(start, ':', (size_t)(end - start));
        if (colon) { host_end = colon; port_start = colon + 1; }
    }
    size_t host_len = (size_t)(host_end - host_start);
    if (!host_len || host_len >= host_size) return -1;
    memcpy(host, host_start, host_len); host[host_len] = '\0';
    if (port_start) {
        size_t len = (size_t)(end - port_start);
        if (!len || len >= port_size) return -1;
        unsigned long value = 0;
        for (size_t i = 0; i < len; i++) {
            if (port_start[i] < '0' || port_start[i] > '9') return -1;
            value = value * 10 + (unsigned)(port_start[i] - '0');
            if (value > 65535) return -1;
        }
        if (!value) return -1;
        memcpy(port, port_start, len); port[len] = '\0';
    } else snprintf(port, port_size, "80");
    int n = snprintf(path, path_size, "%s%s", (*end == '/' ? "" : "/"), end);
    return n < 0 || (size_t)n >= path_size || strchr(path, '#') ? -1 : 0;
}

static int relay_response(int client_fd, int server_fd)
{
    char buf[IO_SIZE];
    int received = 0;
    for (;;) {
        ssize_t n = recv(server_fd, buf, sizeof(buf), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return received ? 0 : -1;
        received = 1;
        if (send_all(client_fd, buf, (size_t)n) < 0) return 0;
    }
}

static void handle_client(int client_fd)
{
    char buf[MAX_HEADERS + 1];
    size_t used = 0;
    char *header_end = NULL;
    while (used < MAX_HEADERS) {
        ssize_t n = recv(client_fd, buf + used, MAX_HEADERS - used, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        used += (size_t)n; buf[used] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (header_end) break;
    }
    if (!header_end) { error_response(client_fd, "431 Request Header Fields Too Large"); return; }
    size_t header_bytes = (size_t)(header_end - buf) + 4;
    char *line_end = strstr(buf, "\r\n");
    if (!line_end) { error_response(client_fd, "400 Bad Request"); return; }
    *line_end = '\0';
    char method[16], url[MAX_URL], version[16], extra;
    if (sscanf(buf, "%15s %8191s %15s %c", method, url, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1"))) {
        error_response(client_fd, "400 Bad Request"); return;
    }
    if (strcmp(method, "GET") && strcmp(method, "POST")) {
        error_response(client_fd, "501 Not Implemented"); return;
    }
    char host[256], port[6], path[MAX_URL];
    if (parse_url(url, host, sizeof(host), port, sizeof(port), path, sizeof(path)) < 0) {
        error_response(client_fd, "400 Bad Request"); return;
    }
    size_t content_length = 0;
    int found_length = 0;
    char *line = line_end + 2;
    while (line < header_end) {
        char *end = strstr(line, "\r\n");
        if (!end || end > header_end) { error_response(client_fd, "400 Bad Request"); return; }
        *end = '\0';
        if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
            error_response(client_fd, "501 Not Implemented"); return;
        }
        if (!strncasecmp(line, "Content-Length:", 15)) {
            char *value = line + 15, *number_end;
            while (*value == ' ' || *value == '\t') value++;
            if (found_length || *value < '0' || *value > '9') { error_response(client_fd, "400 Bad Request"); return; }
            errno = 0;
            unsigned long long length = strtoull(value, &number_end, 10);
            while (*number_end == ' ' || *number_end == '\t') number_end++;
            if (errno == ERANGE || *number_end || length > SIZE_MAX) { error_response(client_fd, "400 Bad Request"); return; }
            content_length = (size_t)length; found_length = 1;
        }
        *end = '\r';
        line = end + 2;
    }
    if (!strcmp(method, "POST") && !found_length) { error_response(client_fd, "411 Length Required"); return; }
    int server_fd = open_clientfd(host, port);
    if (server_fd < 0) { error_response(client_fd, "502 Bad Gateway"); return; }
    struct timeval timeout = {10, 0};
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char out[MAX_HEADERS + MAX_URL];
    int n = snprintf(out, sizeof(out), "%s %s HTTP/1.0\r\nHost: %s%s%s:%s\r\nConnection: close\r\n", method, path,
                     strchr(host, ':') ? "[" : "", host, strchr(host, ':') ? "]" : "", port);
    if (n < 0 || (size_t)n >= sizeof(out)) { close(server_fd); error_response(client_fd, "400 Bad Request"); return; }
    size_t out_len = (size_t)n;
    line = line_end + 2;
    while (line < header_end) {
        char *end = strstr(line, "\r\n");
        size_t line_len = (size_t)(end - line);
        if (strncasecmp(line, "Host:", 5) && strncasecmp(line, "Connection:", 11) &&
            strncasecmp(line, "Proxy-Connection:", 17) && strncasecmp(line, "Proxy-Authorization:", 20)) {
            if (line_len + 2 > sizeof(out) - out_len) { close(server_fd); error_response(client_fd, "431 Request Header Fields Too Large"); return; }
            memcpy(out + out_len, line, line_len + 2); out_len += line_len + 2;
        }
        line = end + 2;
    }
    if (out_len + 2 > sizeof(out)) { close(server_fd); error_response(client_fd, "431 Request Header Fields Too Large"); return; }
    memcpy(out + out_len, "\r\n", 2); out_len += 2;
    if (send_all(server_fd, out, out_len) < 0) { close(server_fd); error_response(client_fd, "502 Bad Gateway"); return; }
    size_t sent_body = used - header_bytes;
    if (sent_body > content_length) sent_body = content_length;
    if (sent_body && send_all(server_fd, buf + header_bytes, sent_body) < 0) { close(server_fd); error_response(client_fd, "502 Bad Gateway"); return; }
    char body[IO_SIZE];
    while (sent_body < content_length) {
        size_t remaining = content_length - sent_body;
        if (remaining > sizeof(body)) remaining = sizeof(body);
        ssize_t count = recv(client_fd, body, remaining, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(server_fd); return; }
        if (send_all(server_fd, body, (size_t)count) < 0) { close(server_fd); error_response(client_fd, "502 Bad Gateway"); return; }
        sent_body += (size_t)count;
    }
    if (relay_response(client_fd, server_fd) < 0) error_response(client_fd, "502 Bad Gateway");
    close(server_fd);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <port>\n", argv[0]); return EXIT_FAILURE; }
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = open_listenfd(argv[1]);
    if (listen_fd < 0) { fprintf(stderr, "Could not open listening socket\n"); return EXIT_FAILURE; }
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        handle_client(client_fd);
        close(client_fd);
    }
    close(listen_fd);
    return EXIT_FAILURE;
}
