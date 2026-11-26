#include <sys/socket.h>
#include <sys/time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "client.h"
#include "reverse_proxy.h"

#define MAXBUF 8192

void reverse_proxy(int fd, HttpRequest *request, const char *backend_host, const char *backend_port)
{
    printf("리버스 프록시 요청: %s\n", request->uri);

    int backend_fd = open_clientfd(backend_host, backend_port);
    if (backend_fd < 0)
    {
        clienterror(fd, request->uri, "502", "Bad Gateway",
                    "Could not connect to backend");
        return;
    }

    struct timeval timeout = {5, 0};
    if (setsockopt(backend_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) < 0 ||
        setsockopt(backend_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        close(backend_fd);
        clienterror(fd, request->uri, "502", "Bad Gateway",
                    "Could not set backend timeout");
        return;
    }
    printf("백엔드 연결 성공\n");
    size_t body_to_send = request->body_received;

    if (body_to_send > request->content_length)
    {
        body_to_send = request->content_length;
    }

    /* Close the upstream connection after this response. */
    const char *line = request->raw_headers;
    const char *end = line + request->headers_length - 2;
    const char *next = strstr(line, "\r\n") + 2;
    int failed = send_all(backend_fd, line, (size_t)(next - line)) < 0;
    for (line = next; line < end && !failed; line = next)
    {
        next = strstr(line, "\r\n") + 2;
        if (strncasecmp(line, "Connection:", 11) && strncasecmp(line, "Keep-Alive:", 11) &&
            strncasecmp(line, "Proxy-Connection:", 17) && strncasecmp(line, "Upgrade:", 8) &&
            strncasecmp(line, "Proxy-Authorization:", 20) && strncasecmp(line, "TE:", 3))
        {
            failed = send_all(backend_fd, line, (size_t)(next - line)) < 0;
        }
    }
    const char connection[] = "Connection: close\r\n\r\n";
    if (failed || send_all(backend_fd, connection, sizeof(connection) - 1) < 0)
    {
        close(backend_fd);
        clienterror(fd, request->uri, "502", "Bad Gateway",
                    "Could not send request to backend");
        return;
    }

    if (body_to_send > 0 &&
        send_all(backend_fd, request->body, body_to_send) < 0)
    {
        close(backend_fd);
        clienterror(fd, request->uri, "502", "Bad Gateway",
                    "Could not send request body");
        return;
    }
    char body_buf[MAXBUF];
    size_t total = body_to_send;
    while (total < request->content_length)
    {
        size_t remaining = request->content_length - total;
        if (remaining > sizeof(body_buf))
        {
            remaining = sizeof(body_buf);
        }
        ssize_t n = recv(fd, body_buf, remaining, 0);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        if (n <= 0)
        {
            close(backend_fd);
            return;
        }
        if (send_all(backend_fd, body_buf, (size_t)n) < 0)
        {
            close(backend_fd);
            clienterror(fd, request->uri, "502", "Bad Gateway",
                        "Could not send request body");
            return;
        }

        total += (size_t)n;
    }

    char response_buf[MAXBUF];
    int response_started = 0;
    int timed_out = 0;

    while (1)
    {
        ssize_t n = recv(backend_fd, response_buf, sizeof(response_buf), 0);

        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        if (n <= 0)
        {
            timed_out = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }

        response_started = 1;

        if (send_all(fd, response_buf, (size_t)n) < 0)
        {
            break;
        }
    }

    close(backend_fd);

    if (!response_started)
    {
        if (timed_out)
            clienterror(fd, request->uri, "504", "Gateway Timeout", "Backend timed out");
        else
            clienterror(fd, request->uri, "502", "Bad Gateway", "No response from backend");
    }

    return;
}
