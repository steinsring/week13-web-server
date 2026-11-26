#include <unistd.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <server.h>
#include <netdb.h>
typedef struct sockaddr SA;

void echo(int connfd);

#define MAXLINE 8192

int main(int argc, char **argv)
{
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    char client_hostname[MAXLINE], client_port[MAXLINE];

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s<port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    listenfd = open_listenfd(argv[1]);
    while (1)
    {
        clientlen = sizeof(struct sockaddr_storage);
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        getnameinfo((SA *)&clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
        printf("Connected to (%s, %s)\n", client_hostname, client_port);
        echo(connfd);
        close(connfd);
    }
    exit(0);
}

// 텍스트 줄을 읽고 echo해주는 echo함수
// recv는 소켓을 통해 상대방이 보낸 데이터를 수신하는 함수
// send는 소켓을 통해 상대방에게 데이터를 전송하는 함수
// 왜 클라이언트에게 데이터를 돌려주는 이유는 지금 만들고있는 서버가 Echo서버이기 때문이야
void echo(int connfd)
{
    ssize_t n;
    char buf[MAXLINE];
    while ((n = recv(connfd, buf, sizeof(buf), 0)) > 0)
    {
        printf("server received %zd bytes\n", n);
        ssize_t sent = 0;
        while (sent < n)
        {
            ssize_t m = send(connfd, buf + sent, n - sent, 0);
            if (m <= 0)
            {
                perror("send");
                return;
            }
            sent += m;
        }
    }
    if (n < 0)
    {
        perror("recv");
    }
}