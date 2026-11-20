#include "http_response.h"

#include <errno.h>
#include <stddef.h>
#include <sys/socket.h>

int http_send_hello(int client)
{
    const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: 11\r\n"
        "Connection: close\r\n"
        "\r\n"
        "hello world";
    size_t sent = 0;

    /* 일부만 보냈다면 아직 보내지 않은 부분부터 이어 보낸다. */
    while (sent < sizeof(response) - 1) {
        ssize_t count = send(client, response + sent, sizeof(response) - 1 - sent, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        sent += (size_t)count;
    }
    return 0;
}
