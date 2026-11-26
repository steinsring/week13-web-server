#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include "client.h"

// 서버에 연결하는 함수

int open_clientfd(const char *hostname, const char *port)
{
    int client_fd;
    struct addrinfo hints, *listp, *p;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    hints.ai_flags |= AI_ADDRCONFIG;
    if (getaddrinfo(hostname, port, &hints, &listp) != 0)
    {
        return -1;
    }

    for (p = listp; p; p = p->ai_next)
    {
        if ((client_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0)
        {
            continue;
        }
        if (connect(client_fd, p->ai_addr, p->ai_addrlen) == 0)
        {
            break;
        }
        close(client_fd);
    }
    int connected = p != NULL;
    freeaddrinfo(listp);
    if (!connected)
    {
        return -1;
    }
    else
    {
        return client_fd;
    }
}
