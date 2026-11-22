#define _POSIX_C_SOURCE 200809L
#include "http_request.h"
#include "http_response.h"
#include "request_io.h"
#include "static_files.h"
#include "concurrency.h"
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/resource.h>

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>


/* 이번 단계에서는 다섯 인자를 모두 요구한다. 예시 수치는 기본값이 아니다. */
static int read_number(const char *text, size_t maximum, size_t *out)
{
    if (!*text) return -1;
    for (const char *p=text; *p; ++p) if (*p<'0'||*p>'9') return -1;
    errno=0; char *end;
    unsigned long long value=strtoull(text,&end,10);
    if(errno||*end||value==0||value>maximum)return -1;
    *out=(size_t)value;return 0;
}

/* socket → bind → listen 순서로 연결을 받을 소켓을 준비한다. */
static int create_listener(int port)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return -1;
    }
    /* 이전 TCP 연결 상태 때문에 서버 재시작이 막히는 상황을 줄인다. */
    int reuse = 1;
    if (setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(server);
        return -1;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server);
        return -1;
    }
    /* 16은 기존 연결 대기열 크기다. 동시 처리 수가 아니다. */
    if (listen(server, 16) < 0) {
        perror("listen");
        close(server);
        return -1;
    }
    return server;
}


/* 이벤트 루프만 소켓을 다루고, pthread 작업 풀은 파일 I/O만 수행한다. */
int main(int argc,char **argv)
{
    size_t port,workers,queue_limit,connection_limit;
    if(argc!=6 || read_number(argv[1],65535,&port) ||
       read_number(argv[3],SIZE_MAX/sizeof(pthread_t),&workers) ||
       read_number(argv[4],SIZE_MAX,&queue_limit) ||
       read_number(argv[5],UINT32_MAX-2,&connection_limit)) {
        fprintf(stderr,"Usage: %s port root workers queue_limit connection_limit\n",argv[0]);
        return EXIT_FAILURE;
    }
    struct rlimit fds;
    if(getrlimit(RLIMIT_NOFILE,&fds)<0 || workers>(SIZE_MAX-connection_limit-16)/4 ||
       connection_limit+workers*4+16>fds.rlim_cur) {
        fprintf(stderr,"Insufficient file descriptor limit\n");return EXIT_FAILURE;
    }
    if(signal(SIGPIPE,SIG_IGN)==SIG_ERR)return EXIT_FAILURE;
    int root=static_root_open(argv[2]);
    if(root<0){perror("root");return EXIT_FAILURE;}
    int listener=create_listener((int)port);
    if(listener<0){close(root);return EXIT_FAILURE;}
    int flags=fcntl(listener,F_GETFL);
    if(flags<0||fcntl(listener,F_SETFL,flags|O_NONBLOCK)<0){close(listener);close(root);return EXIT_FAILURE;}
    int result=concurrent_server_run(listener,root,workers,queue_limit,connection_limit);
    close(listener);close(root);
    return result<0?EXIT_FAILURE:EXIT_SUCCESS;
}
