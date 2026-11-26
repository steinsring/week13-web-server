#include <unistd.h>
#include <client.h>
#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>

#define MAXLINE 8024

int main(int argc, char **argv)
{
    int clientfd;
    char *host, *port, buf[MAXLINE];
    if (argc != 3)
    {
        fprintf(stderr, "usage: %s<host> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    host = argv[1];
    port = argv[2];
    clientfd = open_clientfd(host, port);
    if (clientfd < 0)
    {
        fprintf(stderr, "Connection failed\n");
        exit(EXIT_FAILURE);
    }
    ssize_t n;
    while ((n = recv(clientfd, buf, sizeof(buf), 0)) > 0)
    {
        fwrite(buf, 1, n, stdout);
    }
    if (n < 0)
    {
        perror("recv");
    }
    close(clientfd);
    exit(0);
}