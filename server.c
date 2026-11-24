#define _POSIX_C_SOURCE 200809L
#include "concurrency.h"
#include "http_request.h"
#include "http_response.h"
#include "request_io.h"
#include "static_files.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/* 아홉 인자를 모두 요구한다. 예시 수치는 기본값이 아니다. */
static int read_number(const char* text, size_t maximum, size_t* out)
{
    if (!*text)
        return -1;
    for (const char* p = text; *p; ++p)
        if (*p < '0' || *p > '9')
            return -1;
    errno = 0;
    char* end;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || value == 0 || value > maximum)
        return -1;
    *out = (size_t)value;
    return 0;
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
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);
    if (bind(server, (struct sockaddr*)&address, sizeof(address)) < 0) {
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

typedef struct {
    size_t port;
    const char* root;
    size_t workers;
    size_t queue_limit;
    size_t connection_limit;
    size_t body_limit;
    size_t body_timeout;
    size_t idle_timeout;
    size_t max_requests;
} ServerConfig;

static int read_config(int argc, char** argv, ServerConfig* config)
{
    /* 초를 밀리초로 바꾼 뒤 현재 시각에 더할 여유까지 남겨 둔다. */
    if (argc != 10
        || read_number(argv[1], 65535, &config->port)
        || read_number(argv[3], SIZE_MAX / sizeof(pthread_t), &config->workers)
        || read_number(argv[4], SIZE_MAX, &config->queue_limit)
        || read_number(argv[5], UINT32_MAX - 2, &config->connection_limit)
        || read_number(argv[6], SIZE_MAX, &config->body_limit)
        || read_number(argv[7], INT64_MAX / 2000, &config->body_timeout)
        || read_number(argv[8], INT64_MAX / 2000, &config->idle_timeout)
        || read_number(argv[9], UINT64_MAX, &config->max_requests)) {
        fprintf(stderr, "Usage: %s port root workers queue_limit connection_limit body_limit body_timeout_seconds idle_timeout_seconds max_requests\n", argv[0]);
        return -1;
    }
    config->root = argv[2];
    return 0;
}

static int check_fd_limit(const ServerConfig* config)
{
    struct rlimit fds;
    if (getrlimit(RLIMIT_NOFILE, &fds) < 0 || config->workers > (SIZE_MAX - config->connection_limit - 16) / 4 || config->connection_limit + config->workers * 4 + 16 > fds.rlim_cur) {
        fprintf(stderr, "Insufficient file descriptor limit\n");
        return -1;
    }
    return 0;
}

static int create_nonblocking_listener(int port)
{
    int listener = create_listener(port);
    if (listener < 0)
        return -1;
    int flags = fcntl(listener, F_GETFL);
    if (flags < 0 || fcntl(listener, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(listener);
        return -1;
    }
    return listener;
}

/* 설정 검사 → 루트와 소켓 준비 → 이벤트 루프 실행 → 서버 자원 정리. */
int main(int argc, char** argv)
{
    ServerConfig config;
    if (read_config(argc, argv, &config) < 0 || check_fd_limit(&config) < 0)
        return EXIT_FAILURE;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return EXIT_FAILURE;
    int root = static_root_open(config.root);
    if (root < 0) {
        perror("root");
        return EXIT_FAILURE;
    }
    int listener = create_nonblocking_listener((int)config.port);
    if (listener < 0) {
        close(root);
        return EXIT_FAILURE;
    }
    int result = concurrent_server_run(listener, root, config.workers,
        config.queue_limit, config.connection_limit, config.body_limit, config.body_timeout,
        config.idle_timeout, config.max_requests);
    close(listener);
    close(root);
    return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
