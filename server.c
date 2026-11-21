#define _POSIX_C_SOURCE 200809L
#include "http_request.h"
#include "http_response.h"
#include "request_io.h"
#include "static_files.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/* 포트를 생략하면 8080. 입력했다면 올바른 숫자인지 검사한다. */
static int read_port(int argc, char **argv)
{
    long port = 8080;
    if (argc != 3) {
        fprintf(stderr, "Usage: %s port root\n", argv[0]);
        return -1;
    }
    if (argc == 3) {
        char *end;
        errno = 0;
        port = strtol(argv[1], &end, 10);
        if (errno != 0 || *argv[1] == '\0' || *end != '\0' || port < 1 || port > 65535) {
            fprintf(stderr, "Port must be between 1 and 65535\n");
            return -1;
        }
    }
    return (int)port;
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

static void serve_client(int client, const struct timespec *accepted, int root)
{
    /* 이 요청만의 버퍼·포인터 목록. 다른 요청과 공유하지 않는다. */
    HttpRequest request;
    http_request_init(&request);
    int status = accepted ? http_receive_request(client, &request, accepted) : 500;
    int sent = status == 200 ? static_files_serve(client, root, &request) :
               (status != 0 ? http_send_status(client, status) : 0);
    if (sent < 0)
        perror("send");
    /* 성공·파싱 실패·수신 실패·송신 실패 모두 같은 정리 경로를 거친다. */
    http_request_destroy(&request);
    close(client);
}

int main(int argc, char **argv)
{
    int port = read_port(argc, argv);
    if (port < 0)
        return EXIT_FAILURE;
    /* 끊긴 연결에 써도 SIGPIPE로 서버 전체가 종료되지 않게 한다. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }
    int root = static_root_open(argv[2]);
    if (root < 0) { perror("root"); return EXIT_FAILURE; }
    int server = create_listener(port);
    if (server < 0) { close(root); return EXIT_FAILURE; }
    printf("Listening on port %d\n", port);
    fflush(stdout);

    for (;;) {
        /* 연결이 없으면 잠든다. client는 이번 연결의 통신용 새 소켓이다. */
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            close(server);
            close(root);
            return EXIT_FAILURE;
        }
        struct timespec accepted;
        int timed = clock_gettime(CLOCK_MONOTONIC, &accepted) == 0;
        serve_client(client, timed ? &accepted : NULL, root);
    }
}
