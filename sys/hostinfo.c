#include <socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <socket.h>
#include <netdb.h>

#define MAXLINE 8192

// 해당도메인에 연결된 ip주소들을 출력하는 프로그램
int main(int argc, char **argv)
{
    struct addrinfo *p, *listp, hints;
    char buf[MAXLINE];
    int rc, flags;

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <domain anme>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    // ip주소 리스트 찾기
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ((rc = getaddrinfo(argv[1], NULL, &hints, &listp)) != 0)
    {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(rc));
        exit(EXIT_FAILURE);
    }
    // 리스트에잇는 ip 주소 뽑아주기
    flags = NI_NUMERICHOST;
    for (p = listp; p; p = p->ai_next)
    {
        Getnameinfo(p->ai_addr, p->ai_addrlen, buf, MAXLINE, NULL, 0, flags);
        printf("%s\n", buf);
    }
    Freeaddrinfo(listp);
    exit(0);
}