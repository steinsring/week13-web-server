#define _GNU_SOURCE
#include "concurrency.h"
#include "http_body.h"
#include "http_request.h"
#include "http_response.h"
#include "static_files.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_SIZE 8192
#define LISTENER_KEY UINT64_MAX
#define WAKE_KEY (UINT64_MAX - 1)
#define UPSTREAM_KEY (UINT64_C(1) << 63)
typedef struct Connection Connection;
typedef struct Job Job;
typedef enum { RECEIVING,
    KEEP_ALIVE_IDLE,
    BODY_RECEIVING,
    PROXYING,
    WAITING,
    RETRYING,
    SENDING } Phase;
typedef enum { IDLE,
    QUEUED,
    RUNNING,
    DONE } JobState;
struct Job {
    Job* next;
    Connection* connection;
    JobState state; /* pool.mutex로 보호한다. */
    int first, status, fd;
    off_t remaining, total;
    const char* mime;
    char location[HTTP_HEADER_LIMIT + 4];
    char bytes[CHUNK_SIZE];
    size_t length;
    int64_t queued_at;
    uint64_t request_number;
    uint32_t generation;
};
struct Connection {
    int socket, closed;
    uint32_t slot, generation;
    Phase phase;
    HttpRequest request; /* 내부 문자열 포인터를 위해 객체를 이동하지 않는다. */
    HttpBody body;
    int64_t body_deadline, interim_deadline;
    size_t interim_sent;
    int interim_pending;
    int request_complete, close_after_response;
    uint64_t completed_requests;
    char pending[4096]; /* 한 recv의 잔여 데이터만 유지한다. 응답 중에는 읽지 않는다. */
    size_t pending_length;
    Proxy* proxy;
    uint32_t upstream_generation;
    Job job; /* 연결당 하나: 완료 결과 슬롯도 미리 확보된다. */
    char header[HTTP_HEADER_LIMIT + 1024];
    size_t header_length, header_sent, chunk_sent;
    uint64_t bytes_sent;
    int64_t deadline;
};
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t* threads;
    size_t thread_count, queued, limit;
    int stopping, root, wake;
    Job *head, *tail, *done_head, *done_tail;
} Pool;
typedef struct {
    int epoll, listener, wake, listening, stopping;
    int64_t shutdown_at, accept_retry_at;
    size_t limit, objects;
    size_t body_limit, body_timeout;
    size_t idle_timeout, max_requests;
    ProxyConfig* proxy_config;
    uint32_t upstream_generation;
    Connection** slots;
    uint32_t* generations;
    Pool pool;
} Loop;
static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t signal_wake = -1;
static int mark_shutdown_response(Connection* connection);
static void start_proxy(Loop* loop, Connection* connection);
static void drive_proxy(Loop* loop, Connection* connection, uint32_t events);

static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0)
        abort();
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void notify(int fd)
{
    uint64_t one = 1;
    while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) { }
}
static void stop_signal(int sig)
{
    (void)sig;
    int saved = errno;
    stop_requested = 1;
    if (signal_wake >= 0) {
        uint64_t one = 1;
        (void)write(signal_wake, &one, sizeof(one));
    }
    errno = saved;
}

/* 이 함수에는 소켓 I/O가 없다. worker 하나가 파일 FD와 한 조각만 다룬다. */
static void execute_job(Pool* pool, Job* job)
{
    job->length = 0;
    job->status = 200;
    if (job->first) {
        StaticFileResult result;
        static_files_prepare(pool->root, &job->connection->request, &result);
        job->fd = result.fd;
        job->status = result.status;
        job->remaining = job->total = result.size;
        job->mime = result.mime;
        memcpy(job->location, result.location, sizeof(job->location));
        if (job->status != 200)
            return;
    }
    if (job->remaining == 0)
        return;
    size_t want = job->remaining > (off_t)CHUNK_SIZE ? CHUNK_SIZE : (size_t)job->remaining;
    ssize_t n;
    do {
        n = read(job->fd, job->bytes, want);
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        job->status = 500;
        return;
    }
    job->length = (size_t)n;
    job->remaining -= n;
}
/* 대기열에서 하나를 꺼낸다. 종료 중이어도 이미 대기 중인 작업은 처리한다. */
static Job* take_job(Pool* pool, int* expired)
{
    pthread_mutex_lock(&pool->mutex);
    while (!pool->head && !pool->stopping)
        pthread_cond_wait(&pool->ready, &pool->mutex);
    if (!pool->head) {
        pthread_mutex_unlock(&pool->mutex);
        return NULL;
    }
    Job* job = pool->head;
    pool->head = job->next;
    if (!pool->head)
        pool->tail = NULL;
    --pool->queued;
    *expired = now_ms() - job->queued_at >= 10000;
    job->state = RUNNING;
    pthread_mutex_unlock(&pool->mutex);
    notify(pool->wake);
    return job;
}

static void publish_completed_job(Pool* pool, Job* job)
{
    pthread_mutex_lock(&pool->mutex);
    job->state = DONE;
    job->next = NULL;
    if (pool->done_tail)
        pool->done_tail->next = job;
    else
        pool->done_head = job;
    pool->done_tail = job;
    pthread_mutex_unlock(&pool->mutex);
    notify(pool->wake);
}

/* 작업 인수 → 파일 처리 → 완료 알림. 소켓은 이벤트 루프만 다룬다. */
static void* worker(void* argument)
{
    Pool* pool = argument;
    for (;;) {
        int expired;
        Job* job = take_job(pool, &expired);
        if (!job)
            return NULL;
        if (expired) {
            job->status = 503;
            job->length = 0;
        } else {
            execute_job(pool, job);
        }
        publish_completed_job(pool, job);
    }
}
static int pool_init(Pool* pool, size_t count, size_t limit, int root, int wake)
{
    memset(pool, 0, sizeof(*pool));
    pool->limit = limit;
    pool->root = root;
    pool->wake = wake;
    if (pthread_mutex_init(&pool->mutex, NULL))
        return -1;
    if (pthread_cond_init(&pool->ready, NULL)) {
        pthread_mutex_destroy(&pool->mutex);
        return -1;
    }
    pool->threads = calloc(count, sizeof(*pool->threads));
    if (!pool->threads)
        goto failure;
    for (; pool->thread_count < count; ++pool->thread_count)
        if (pthread_create(&pool->threads[pool->thread_count], NULL, worker, pool))
            goto failure;
    return 0;
failure:
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->ready);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->thread_count; ++i)
        pthread_join(pool->threads[i], NULL);
    free(pool->threads);
    pthread_cond_destroy(&pool->ready);
    pthread_mutex_destroy(&pool->mutex);
    return -1;
}
static JobState job_state(Pool* pool, Job* job)
{
    pthread_mutex_lock(&pool->mutex);
    JobState state = job->state;
    pthread_mutex_unlock(&pool->mutex);
    return state;
}
static int submit(Pool* pool, Job* job)
{
    int ok = 0;
    pthread_mutex_lock(&pool->mutex);
    if (pool->queued < pool->limit) {
        job->next = NULL;
        job->state = QUEUED;
        if (pool->tail)
            pool->tail->next = job;
        else
            pool->head = job;
        pool->tail = job;
        ++pool->queued;
        ok = 1;
        pthread_cond_signal(&pool->ready);
    }
    pthread_mutex_unlock(&pool->mutex);
    return ok;
}
/* 아직 실행되지 않은 작업만 제거한다. RUNNING의 파일/요청은 만지지 않는다. */
static int cancel_queued(Pool* pool, Job* job)
{
    int canceled = 0;
    pthread_mutex_lock(&pool->mutex);
    if (job->state == QUEUED) {
        Job **at = &pool->head, *prev = NULL;
        while (*at && *at != job) {
            prev = *at;
            at = &(*at)->next;
        }
        if (*at) {
            *at = job->next;
            if (pool->tail == job)
                pool->tail = prev;
            --pool->queued;
            job->state = IDLE;
            canceled = 1;
        }
    }
    pthread_mutex_unlock(&pool->mutex);
    return canceled;
}
static uint64_t key(Connection* connection) { return ((uint64_t)connection->generation << 32) | connection->slot; }
static int interest(Loop* loop, Connection* connection, uint32_t events)
{
    struct epoll_event e = { .events = events | (events & EPOLLIN ? EPOLLRDHUP : 0), .data.u64 = key(connection) };
    return epoll_ctl(loop->epoll, EPOLL_CTL_MOD, connection->socket, &e);
}
static void release_connection(Loop* loop, Connection* connection)
{
    proxy_destroy(connection->proxy);
    if (connection->job.fd >= 0)
        close(connection->job.fd);
    http_request_destroy(&connection->request);
    http_body_destroy(&connection->body);
    loop->slots[connection->slot] = NULL;
    --loop->objects;
    free(connection);
}
static void close_connection(Loop* loop, Connection* connection)
{
    if (connection->closed)
        return;
    connection->closed = 1;
    proxy_destroy(connection->proxy);
    connection->proxy = NULL;
    epoll_ctl(loop->epoll, EPOLL_CTL_DEL, connection->socket, NULL);
    close(connection->socket);
    JobState state = job_state(&loop->pool, &connection->job);
    if (state == QUEUED && cancel_queued(&loop->pool, &connection->job))
        state = IDLE;
    else if (state == QUEUED)
        state = job_state(&loop->pool, &connection->job);
    /* DONE도 완료 큐에서 꺼낼 때까지 참조를 보존한다. */
    if (state == IDLE)
        release_connection(loop, connection);
}

/* Connection의 여러 줄과 쉼표 목록 중 close 하나만 있어도 종료가 우선이다. */
static int client_requests_close(const HttpRequest* request)
{
    size_t cursor = 0;
    const HttpHeader* header;
    while ((header = http_request_next_header(request, "Connection", &cursor))) {
        const char* p = header->value;
        while (*p) {
            while (*p == ' ' || *p == '\t' || *p == ',')
                ++p;
            const char* start = p;
            while (*p && *p != ',')
                ++p;
            const char* end = p;
            while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
                --end;
            if (end - start == 5 && !strncasecmp(start, "close", 5))
                return 1;
        }
    }
    return 0;
}

static int response_must_close(Loop* loop, Connection* connection)
{
    return connection->close_after_response || loop->stopping || stop_requested
        || connection->completed_requests + 1 >= loop->max_requests;
}

/* 현재 요청 경계를 넘은 바이트는 아직 파싱하지 않고 다음 차례까지 보관한다. */
static void save_pending(Connection* connection, const char* data, size_t length)
{
    if (length)
        memcpy(connection->pending, data, length);
    connection->pending_length = length;
}

static void error_response(Loop* loop, Connection* connection, int status)
{
    if (connection->closed)
        return;
    if (connection->bytes_sent) {
        close_connection(loop, connection);
        return;
    }
    if (!connection->request_complete || (status != 403 && status != 404))
        connection->close_after_response = 1;
    /* 100을 일부 보냈으면 끝까지 보낸 뒤 최종 오류를 붙인다.
     * 한 바이트도 보내지 않았다면 임시 응답은 취소할 수 있다. */
    if (!connection->interim_sent)
        connection->interim_pending = 0;
    if (connection->job.fd >= 0) {
        close(connection->job.fd);
        connection->job.fd = -1;
    }
    const char* body = http_status_reason(status);
    connection->job.length = strlen(body);
    memcpy(connection->job.bytes, body, connection->job.length);
    connection->job.remaining = 0;
    int n = http_format_header_policy(connection->header, sizeof(connection->header), status,
        "text/plain; charset=utf-8", (off_t)connection->job.length, NULL, response_must_close(loop, connection));
    if (n < 0) {
        close_connection(loop, connection);
        return;
    }
    connection->header_length = (size_t)n;
    connection->header_sent = connection->chunk_sent = 0;
    connection->phase = SENDING;
    connection->deadline = now_ms() + 10000;
    if (interest(loop, connection, EPOLLOUT) < 0)
        close_connection(loop, connection);
}
static void schedule_read(Loop* loop, Connection* connection, int first)
{
    connection->job.request_number = connection->completed_requests + 1;
    connection->job.generation = connection->generation;
    connection->job.first = first;
    connection->job.queued_at = now_ms();
    connection->deadline = connection->job.queued_at + 10000;
    connection->phase = WAITING;
    if (interest(loop, connection, 0) < 0) {
        close_connection(loop, connection);
        return;
    }
    if (!submit(&loop->pool, &connection->job)) {
        if (first)
            error_response(loop, connection, 503);
        else
            connection->phase = RETRYING;
    }
}
static void retry_waiters(Loop* loop)
{
    for (;;) {
        Connection* oldest = NULL;
        for (size_t i = 0; i < loop->limit; ++i) {
            Connection* connection = loop->slots[i];
            if (connection && !connection->closed && connection->phase == RETRYING && (!oldest || connection->job.queued_at < oldest->job.queued_at))
                oldest = connection;
        }
        if (!oldest)
            return;
        if (now_ms() >= oldest->deadline) {
            error_response(loop, oldest, 503);
            continue;
        }
        if (!submit(&loop->pool, &oldest->job))
            return;
        oldest->phase = WAITING;
    }
}
static void completed_jobs(Loop* loop)
{
    Pool* pool = &loop->pool;
    pthread_mutex_lock(&pool->mutex);
    Job* jobs = pool->done_head;
    pool->done_head = pool->done_tail = NULL;
    pthread_mutex_unlock(&pool->mutex);
    while (jobs) {
        Job* job = jobs;
        jobs = job->next;
        Connection* connection = job->connection;
        pthread_mutex_lock(&pool->mutex);
        job->state = IDLE;
        pthread_mutex_unlock(&pool->mutex);
        if (connection->closed) {
            release_connection(loop, connection);
            continue;
        }
        /* 소켓 세대와 요청 순번을 모두 확인해 늦은 결과의 적용 대상을 제한한다. */
        if (job->generation != connection->generation
            || job->request_number != connection->completed_requests + 1) {
            close_connection(loop, connection);
            continue;
        }
        if (job->status != 200 && job->status != 301) {
            error_response(loop, connection, job->status);
            continue;
        }
        connection->chunk_sent = 0;
        if (job->first) {
            int n = http_format_header_policy(connection->header, sizeof(connection->header), job->status,
                job->mime, job->total, job->status == 301 ? job->location : NULL, response_must_close(loop, connection));
            if (n < 0) {
                error_response(loop, connection, 500);
                continue;
            }
            connection->header_length = (size_t)n;
            connection->header_sent = 0;
        }
        connection->phase = SENDING;
        connection->deadline = now_ms() + 10000;
        if (interest(loop, connection, EPOLLOUT) < 0)
            close_connection(loop, connection);
    }
    retry_waiters(loop);
}
static int parse_status(HttpParseResult result)
{
    switch (result) {
    case HTTP_PARSE_INVALID:
        return 400;
    case HTTP_PARSE_TOO_LARGE:
        return 431;
    case HTTP_PARSE_NO_MEMORY:
        return 500;
    case HTTP_PARSE_UNSUPPORTED_VERSION:
        return 505;
    default:
        return 500;
    }
}
/* POST는 파일 작업 없이 최종 응답만 준비한다. 본문은 이번 요청 정리까지 보관한다. */
static void complete_post(Loop* loop, Connection* connection)
{
    connection->request_complete = 1;
    if (proxy_route(connection->request.path)) {
        start_proxy(loop, connection);
        return;
    }
    int n = http_format_no_content_policy(connection->header, sizeof(connection->header), response_must_close(loop, connection));
    if (n < 0) {
        error_response(loop, connection, 500);
        return;
    }
    if (!connection->interim_sent)
        connection->interim_pending = 0;
    connection->header_length = (size_t)n;
    connection->header_sent = connection->chunk_sent = 0;
    connection->job.length = 0;
    connection->job.remaining = 0;
    connection->phase = SENDING;
    connection->deadline = now_ms() + 10000;
    if (interest(loop, connection, EPOLLOUT) < 0)
        close_connection(loop, connection);
}

static int receive_body_bytes(Loop* loop, Connection* connection, const char* data, size_t length)
{
    if (now_ms() >= connection->body_deadline) {
        error_response(loop, connection, 408);
        return 0;
    }
    http_body_feed(&connection->body, data, length);
    if (connection->body.received == connection->body.length) {
        complete_post(loop, connection);
        return 0;
    }
    return 1;
}

/* 헤더 경계 검증 → 메서드 선택 → 메모리 확보 → 같은 recv에 붙은 본문 처리. */
static void finish_request_headers(Loop* loop, Connection* connection, const char* data, size_t length)
{
    int64_t completed_at = now_ms();
    if (completed_at >= connection->deadline) {
        error_response(loop, connection, 408);
        return;
    }
    size_t body_length;
    int expect_continue;
    int status = http_body_validate(&connection->request, loop->body_limit, &body_length, &expect_continue);
    if (status) {
        error_response(loop, connection, status);
        return;
    }
    connection->close_after_response = client_requests_close(&connection->request);
    if (!strcmp(connection->request.method, "GET")) {
        connection->request_complete = 1;
        save_pending(connection, data, length);
        if (proxy_route(connection->request.path)) {
            start_proxy(loop, connection);
            return;
        }
        retry_waiters(loop);
        schedule_read(loop, connection, 1);
        return;
    }
    status = http_body_allocate(&connection->body, body_length);
    if (status) {
        error_response(loop, connection, status);
        return;
    }
    connection->body_deadline = completed_at + (int64_t)loop->body_timeout * 1000;
    connection->phase = BODY_RECEIVING;
    size_t consumed = length < body_length ? length : body_length;
    save_pending(connection, data + consumed, length - consumed);
    if (body_length == 0) {
        complete_post(loop, connection);
        return;
    }
    if (!receive_body_bytes(loop, connection, data, consumed))
        return;
    if (expect_continue) {
        connection->interim_pending = 1;
        connection->interim_deadline = now_ms() + 10000;
    }
    if (interest(loop, connection, EPOLLIN | (connection->interim_pending ? EPOLLOUT : 0)) < 0)
        close_connection(loop, connection);
}

/* 헤더/본문 경계까지만 소비한다. 다음 요청은 응답 완료 전까지 파싱하지 않는다. */
static int consume_received(Loop* loop, Connection* connection, const char* data, size_t length)
{
    if (connection->phase == BODY_RECEIVING) {
        size_t remaining = connection->body.length - connection->body.received;
        size_t consumed = length < remaining ? length : remaining;
        save_pending(connection, data + consumed, length - consumed);
        return receive_body_bytes(loop, connection, data, consumed);
    }
    if (now_ms() >= connection->deadline) {
        error_response(loop, connection, 408);
        return 0;
    }
    size_t consumed;
    HttpParseResult result = http_request_feed_bytes(&connection->request, data, length, &consumed);
    if (result == HTTP_PARSE_INCOMPLETE)
        return 1;
    if (result != HTTP_PARSE_COMPLETE) {
        error_response(loop, connection, parse_status(result));
        return 0;
    }
    finish_request_headers(loop, connection, data + consumed, length - consumed);
    return 0;
}

static void receive_connection(Loop* loop, Connection* connection)
{
    char data[4096];
    /* LT epoll로 남은 데이터는 다시 통지된다. 한 연결의 수신량을 유한하게 제한한다. */
    for (unsigned turn = 0; turn < 16; ++turn) {
        int64_t deadline = connection->phase == BODY_RECEIVING ? connection->body_deadline : connection->deadline;
        if (now_ms() >= deadline) {
            if (connection->phase == KEEP_ALIVE_IDLE)
                close_connection(loop, connection);
            else
                error_response(loop, connection, 408);
            return;
        }
        ssize_t n;
        if (connection->pending_length) {
            n = (ssize_t)connection->pending_length;
            memcpy(data, connection->pending, (size_t)n);
            connection->pending_length = 0;
        } else
            n = recv(connection->socket, data, sizeof(data), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            close_connection(loop, connection);
            return;
        }
        if (n == 0) {
            if (connection->request.used || connection->phase == BODY_RECEIVING)
                error_response(loop, connection, 400);
            else
                close_connection(loop, connection);
            return;
        }
        if (connection->phase == KEEP_ALIVE_IDLE) {
            if (now_ms() >= connection->deadline) {
                close_connection(loop, connection);
                return;
            }
            connection->phase = RECEIVING;
            connection->deadline = now_ms() + 10000;
        }
        if (!consume_received(loop, connection, data, (size_t)n))
            return;
    }
}

/* 임시 응답 진행량은 최종 bytes_sent와 분리한다. 부분 100 뒤에도 400/408이 가능하다.
 * 1은 완료, 0은 EAGAIN, -1은 연결 종료다. */
static int send_continue(Loop* loop, Connection* connection)
{
    static const char response[] = "HTTP/1.1 100 Continue\r\n\r\n";
    while (connection->interim_pending && connection->interim_sent < sizeof(response) - 1) {
        if (now_ms() >= connection->interim_deadline) {
            close_connection(loop, connection);
            return -1;
        }
        ssize_t n = send(connection->socket, response + connection->interim_sent,
            sizeof(response) - 1 - connection->interim_sent, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n <= 0) {
            close_connection(loop, connection);
            return -1;
        }
        connection->interim_sent += (size_t)n;
        connection->interim_deadline = now_ms() + 10000;
    }
    connection->interim_pending = 0;
    return 1;
}

static void send_body_continue(Loop* loop, Connection* connection)
{
    if (now_ms() >= connection->body_deadline) {
        error_response(loop, connection, 408);
        return;
    }
    if (send_continue(loop, connection) != 1)
        return;
    if (interest(loop, connection, EPOLLIN) < 0)
        close_connection(loop, connection);
}

/* 요청 소유 자원만 정리한다. 소켓·세대·횟수·잔여 데이터는 연결이 소유한다. */
static void reset_request(Connection* connection)
{
    proxy_destroy(connection->proxy);
    connection->proxy = NULL;
    if (connection->job.fd >= 0)
        close(connection->job.fd);
    http_request_destroy(&connection->request);
    http_body_destroy(&connection->body);
    http_request_init(&connection->request);
    memset(&connection->job, 0, sizeof(connection->job));
    connection->job.fd = -1;
    connection->job.connection = connection;
    connection->header_length = connection->header_sent = connection->chunk_sent = 0;
    connection->bytes_sent = 0;
    connection->interim_pending = 0;
    connection->interim_sent = 0;
    connection->request_complete = connection->close_after_response = 0;
    connection->deadline = connection->body_deadline = connection->interim_deadline = 0;
}

static void finish_response(Loop* loop, Connection* connection)
{
    int close = response_must_close(loop, connection);
    ++connection->completed_requests;
    if (close) {
        close_connection(loop, connection);
        return;
    }
    reset_request(connection);
    int buffered = connection->pending_length != 0;
    connection->phase = buffered ? RECEIVING : KEEP_ALIVE_IDLE;
    connection->deadline = now_ms() + (buffered ? 10000 : (int64_t)loop->idle_timeout * 1000);
    if (interest(loop, connection, EPOLLIN) < 0) {
        close_connection(loop, connection);
        return;
    }
    if (buffered)
        receive_connection(loop, connection);
}

static void proxy_failure(Loop* loop, Connection* connection, int status)
{
    proxy_destroy(connection->proxy);
    connection->proxy = NULL;
    connection->close_after_response = 1;
    error_response(loop, connection, status);
}

/* upstream별 새 세대를 써서 이전 요청의 늦은 epoll 이벤트를 무시한다. */
static int watch_upstream(Loop* loop, Connection* connection)
{
    int fd = proxy_socket(connection->proxy);
    if (fd < 0)
        return 0;
    uint32_t events = proxy_interest(connection->proxy);
    /* 감시 0도 HUP/ERR를 반복 통지할 수 있다. 백프레셔 동안에는 등록 자체를 뺀다. */
    if (!events) {
        if (epoll_ctl(loop->epoll, EPOLL_CTL_DEL, fd, NULL) == 0 || errno == ENOENT)
            return 0;
        return -1;
    }
    struct epoll_event event = {
        .events = events,
        .data.u64 = UPSTREAM_KEY | ((uint64_t)connection->upstream_generation << 32) | connection->slot
    };
    if (epoll_ctl(loop->epoll, EPOLL_CTL_MOD, fd, &event) == 0)
        return 0;
    if (errno != ENOENT)
        return -1;
    return epoll_ctl(loop->epoll, EPOLL_CTL_ADD, fd, &event);
}

static void synchronize_proxy(Loop* loop, Connection* connection)
{
    Proxy* proxy = connection->proxy;
    connection->close_after_response |= proxy_frontend_close(proxy);
    if (proxy_finished(proxy)) {
        finish_response(loop, connection);
        return;
    }
    size_t length;
    int final;
    proxy_output(proxy, &length, &final);
    if (length && !connection->deadline)
        connection->deadline = now_ms() + 10000;
    if (!length)
        connection->deadline = 0;
    if (watch_upstream(loop, connection) < 0
        || interest(loop, connection, length || connection->interim_pending ? EPOLLOUT : 0) < 0)
        close_connection(loop, connection);
}

static void drive_proxy(Loop* loop, Connection* connection, uint32_t events)
{
    int status = proxy_step(connection->proxy, events, now_ms());
    if (status) {
        proxy_failure(loop, connection, status);
        return;
    }
    synchronize_proxy(loop, connection);
}

static void start_proxy(Loop* loop, Connection* connection)
{
    int status;
    connection->proxy = proxy_start(loop->proxy_config, &connection->request, &connection->body,
        response_must_close(loop, connection), now_ms(), &status);
    if (!connection->proxy) {
        connection->close_after_response = 1;
        error_response(loop, connection, status);
        return;
    }
    connection->upstream_generation = (++loop->upstream_generation) & UINT32_C(0x7fffffff);
    connection->phase = PROXYING;
    connection->deadline = 0;
    drive_proxy(loop, connection, 0);
}

/* 로컬 100·백엔드 임시 응답·최종 응답의 바이트 진행량을 분리한다. */
static void send_proxy(Loop* loop, Connection* connection)
{
    if (stop_requested && proxy_mark_close(connection->proxy) < 0) {
        close_connection(loop, connection);
        return;
    }
    if (connection->interim_pending) {
        if (send_continue(loop, connection) != 1)
            return;
        connection->deadline = now_ms() + 10000;
    }
    size_t length;
    int final;
    const char* data = proxy_output(connection->proxy, &length, &final);
    if (!length) {
        drive_proxy(loop, connection, 0);
        return;
    }
    ssize_t n = send(connection->socket, data, length, 0);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    if (n <= 0) {
        close_connection(loop, connection);
        return;
    }
    if (final)
        connection->bytes_sent += (size_t)n;
    connection->deadline = now_ms() + 10000;
    proxy_consume_output(connection->proxy, (size_t)n, now_ms());
    drive_proxy(loop, connection, 0);
}

static void send_connection(Loop* loop, Connection* connection)
{
    if (stop_requested && mark_shutdown_response(connection) < 0) {
        close_connection(loop, connection);
        return;
    }
    if (connection->interim_pending) {
        if (send_continue(loop, connection) != 1)
            return;
        connection->deadline = now_ms() + 10000;
    }
    /* 이 연결의 현재 한 조각만 송신한다. 다음 파일 읽기는 모두 보낸 뒤 제출한다. */
    while (connection->header_sent < connection->header_length || connection->chunk_sent < connection->job.length) {
        int header = connection->header_sent < connection->header_length;
        const char* data = header ? connection->header + connection->header_sent : connection->job.bytes + connection->chunk_sent;
        size_t length = header ? connection->header_length - connection->header_sent : connection->job.length - connection->chunk_sent;
        ssize_t n = send(connection->socket, data, length, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            close_connection(loop, connection);
            return;
        }
        if (n == 0) {
            close_connection(loop, connection);
            return;
        }
        connection->bytes_sent += (size_t)n;
        connection->deadline = now_ms() + 10000;
        if (header)
            connection->header_sent += (size_t)n;
        else
            connection->chunk_sent += (size_t)n;
    }
    if (connection->job.remaining > 0) {
        retry_waiters(loop);
        schedule_read(loop, connection, 0);
    } else
        finish_response(loop, connection);
}
static void monitor_listener(Loop* loop)
{
    int enabled = !loop->stopping && loop->objects < loop->limit && now_ms() >= loop->accept_retry_at;
    if (enabled == loop->listening)
        return;
    struct epoll_event e = { .events = EPOLLIN, .data.u64 = LISTENER_KEY };
    if (epoll_ctl(loop->epoll, enabled ? EPOLL_CTL_ADD : EPOLL_CTL_DEL, loop->listener, &e) == 0)
        loop->listening = enabled;
}
/* 반환한 객체는 슬롯에 고정해 요청 내부 포인터의 주소를 보존한다. */
static Connection* create_connection(Loop* loop, int socket)
{
    size_t slot = 0;
    while (loop->slots[slot])
        ++slot;
    Connection* connection = calloc(1, sizeof(*connection));
    if (!connection)
        return NULL;
    connection->socket = socket;
    connection->slot = (uint32_t)slot;
    connection->generation = (++loop->generations[slot]) & UINT32_C(0x7fffffff);
    connection->job.fd = -1;
    connection->job.connection = connection;
    connection->phase = RECEIVING;
    connection->deadline = now_ms() + 10000;
    http_request_init(&connection->request);
    loop->slots[slot] = connection;
    ++loop->objects;
    return connection;
}

static void watch_new_connection(Loop* loop, Connection* connection)
{
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLRDHUP,
        .data.u64 = key(connection)
    };
    if (epoll_ctl(loop->epoll, EPOLL_CTL_ADD, connection->socket, &event) < 0)
        close_connection(loop, connection);
}

static void accept_connections(Loop* loop)
{
    while (!loop->stopping && loop->objects < loop->limit) {
        int fd = accept4(loop->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept");
                loop->accept_retry_at = now_ms() + 1000;
            }
            break;
        }
        Connection* connection = create_connection(loop, fd);
        if (!connection) {
            close(fd);
            loop->accept_retry_at = now_ms() + 1000;
            break;
        }
        watch_new_connection(loop, connection);
    }
    monitor_listener(loop);
}
static int next_timeout(Loop* loop)
{
    int64_t now = now_ms(), nearest = now + 1000;
    if (loop->stopping && loop->shutdown_at < nearest)
        nearest = loop->shutdown_at;
    if (loop->accept_retry_at > now && loop->accept_retry_at < nearest)
        nearest = loop->accept_retry_at;
    for (size_t i = 0; i < loop->limit; ++i) {
        Connection* connection = loop->slots[i];
        if (!connection || connection->closed)
            continue;
        if (connection->phase == WAITING && job_state(&loop->pool, &connection->job) != QUEUED)
            continue;
        if (connection->phase == PROXYING) {
            int64_t upstream = proxy_deadline(connection->proxy);
            if (upstream < nearest)
                nearest = upstream;
            if (connection->deadline && !connection->interim_pending && connection->deadline < nearest)
                nearest = connection->deadline;
            if (connection->interim_pending && connection->interim_deadline < nearest)
                nearest = connection->interim_deadline;
            continue;
        }
        int64_t deadline = connection->phase == BODY_RECEIVING ? connection->body_deadline : connection->deadline;
        if (connection->phase == SENDING && connection->interim_pending)
            deadline = connection->interim_deadline;
        if (deadline < nearest)
            nearest = deadline;
        if (connection->interim_pending && connection->interim_deadline < nearest)
            nearest = connection->interim_deadline;
    }
    return nearest <= now ? 0 : (int)(nearest - now);
}
static void expire_connections(Loop* loop)
{
    int64_t now = now_ms();
    for (size_t i = 0; i < loop->limit; ++i) {
        Connection* connection = loop->slots[i];
        if (!connection || connection->closed)
            continue;
        if (connection->interim_pending && now >= connection->interim_deadline) {
            close_connection(loop, connection);
            continue;
        }
        if (connection->phase == BODY_RECEIVING) {
            if (now >= connection->body_deadline)
                error_response(loop, connection, 408);
            continue;
        }
        if (connection->phase == PROXYING) {
            if (connection->deadline && !connection->interim_pending && now >= connection->deadline)
                close_connection(loop, connection);
            else if (now >= proxy_deadline(connection->proxy))
                drive_proxy(loop, connection, 0);
            continue;
        }
        if (connection->phase == SENDING && connection->interim_pending)
            continue;
        if (now < connection->deadline)
            continue;
        if (connection->phase == RECEIVING)
            error_response(loop, connection, 408);
        else if (connection->phase == KEEP_ALIVE_IDLE)
            close_connection(loop, connection);
        else if (connection->phase == SENDING)
            close_connection(loop, connection);
        else if (connection->phase == RETRYING)
            error_response(loop, connection, 503);
        else if (cancel_queued(&loop->pool, &connection->job))
            error_response(loop, connection, 503);
    }
}
/* epoll이 돌려준 한 이벤트를 연결 종류와 현재 단계에 따라 처리한다. */
static void dispatch_event(Loop* loop, const struct epoll_event* event)
{
    uint64_t id = event->data.u64;
    if (id == WAKE_KEY) {
        uint64_t value;
        while (read(loop->wake, &value, sizeof(value)) > 0) { };
        return;
    }
    if (id == LISTENER_KEY) {
        if (!stop_requested)
            accept_connections(loop);
        return;
    }
    uint32_t slot = (uint32_t)id, gen = (uint32_t)(id >> 32) & UINT32_C(0x7fffffff);
    if (slot >= loop->limit)
        return;
    Connection* connection = loop->slots[slot];
    if (!connection || connection->closed)
        return;
    if (id & UPSTREAM_KEY) {
        if (connection->proxy && connection->upstream_generation == gen)
            drive_proxy(loop, connection, event->events);
        return;
    }
    if (connection->generation != gen)
        return;
    uint32_t e = event->events;
    if (stop_requested && connection->phase == KEEP_ALIVE_IDLE) {
        close_connection(loop, connection);
        return;
    }
    if (e & (EPOLLERR | EPOLLHUP)) {
        close_connection(loop, connection);
        return;
    }
    if ((connection->phase == RECEIVING || connection->phase == BODY_RECEIVING || connection->phase == KEEP_ALIVE_IDLE) && (e & (EPOLLIN | EPOLLRDHUP)))
        receive_connection(loop, connection);
    else if (connection->phase == BODY_RECEIVING && (e & EPOLLOUT))
        send_body_continue(loop, connection);
    else if (connection->phase == SENDING && (e & EPOLLOUT))
        send_connection(loop, connection);
    else if (connection->phase == PROXYING && (e & EPOLLOUT))
        send_proxy(loop, connection);
    /* 대기 중 RDHUP는 상대의 write-half close일 수 있어 송신 가능성을 유지한다. */
}

static void force_shutdown(Loop* loop)
{
    /* 실행 중인 파일 I/O를 강제로 취소하지 않고 프로세스 전체를 종료한다. */
    for (size_t i = 0; i < loop->limit; ++i)
        if (loop->slots[i] && !loop->slots[i]->closed) {
            proxy_destroy(loop->slots[i]->proxy);
            close(loop->slots[i]->socket);
        }
    _exit(EXIT_SUCCESS);
}

/* 아직 헤더를 한 바이트도 보내지 않았을 때만 종료 표시를 덧붙일 수 있다. */
static int mark_shutdown_response(Connection* connection)
{
    connection->close_after_response = 1;
    if (connection->proxy)
        return proxy_mark_close(connection->proxy);
    if (connection->header_sent || !connection->header_length
        || strstr(connection->header, "Connection: close\r\n"))
        return 0;
    static const char close_header[] = "Connection: close\r\n";
    size_t n = sizeof(close_header) - 1;
    if (connection->header_length < 2 || connection->header_length + n >= sizeof(connection->header))
        return -1;
    size_t insertion = connection->header_length - 2;
    memmove(connection->header + insertion + n, connection->header + insertion, 3);
    memcpy(connection->header + insertion, close_header, n);
    connection->header_length += n;
    return 0;
}

static void begin_shutdown(Loop* loop)
{
    loop->stopping = 1;
    loop->shutdown_at = now_ms() + 10000;
    monitor_listener(loop);
    for (size_t i = 0; i < loop->limit; ++i) {
        Connection* connection = loop->slots[i];
        if (!connection || connection->closed)
            continue;
        if (connection->phase == KEEP_ALIVE_IDLE)
            close_connection(loop, connection);
        else if (mark_shutdown_response(connection) < 0)
            close_connection(loop, connection);
    }
}

static void update_loop_state(Loop* loop)
{
    if (stop_requested && !loop->stopping)
        begin_shutdown(loop);
    completed_jobs(loop);
    expire_connections(loop);
    retry_waiters(loop);
    monitor_listener(loop);
}

/* 상태 정리 → 준비 이벤트 대기 → 이벤트별 처리 순서를 반복한다. */
static void run_event_loop(Loop* loop)
{
    for (;;) {
        update_loop_state(loop);
        if (loop->stopping && loop->objects == 0)
            return;
        if (loop->stopping && now_ms() >= loop->shutdown_at)
            force_shutdown(loop);
        struct epoll_event events[64];
        int count = epoll_wait(loop->epoll, events, 64, next_timeout(loop));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait");
            loop->stopping = 1;
            loop->shutdown_at = now_ms();
            continue;
        }
        for (int i = 0; i < count; ++i)
            dispatch_event(loop, &events[i]);
    }
}

static int allocate_loop_resources(Loop* loop, size_t limit)
{
    loop->epoll = -1;
    loop->wake = -1;
    /* 요청별 upstream 버퍼까지 포함한 크기 계산이 넘치지 않아야 한다. */
    if (proxy_connection_size() > SIZE_MAX - sizeof(Connection)
        || limit > SIZE_MAX / (sizeof(Connection) + proxy_connection_size()))
        return -1;
    loop->limit = limit;
    loop->slots = calloc(limit, sizeof(*loop->slots));
    loop->generations = calloc(limit, sizeof(*loop->generations));
    if (!loop->slots || !loop->generations)
        return -1;
    loop->epoll = epoll_create1(EPOLL_CLOEXEC);
    loop->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    return loop->epoll < 0 || loop->wake < 0 ? -1 : 0;
}

static void release_loop_resources(Loop* loop)
{
    if (loop->wake >= 0)
        close(loop->wake);
    if (loop->epoll >= 0)
        close(loop->epoll);
    free(loop->slots);
    free(loop->generations);
}

static void stop_pool(Pool* pool)
{
    pthread_mutex_lock(&pool->mutex);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->ready);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->thread_count; ++i)
        pthread_join(pool->threads[i], NULL);
    free(pool->threads);
    pthread_cond_destroy(&pool->ready);
    pthread_mutex_destroy(&pool->mutex);
}

int concurrent_server_run(int listener, int root, size_t workers, size_t queue_limit, size_t limit,
    size_t body_limit, size_t body_timeout, size_t idle_timeout, size_t max_requests, ProxyConfig* proxy_config)
{
    Loop loop = { .listener = listener, .body_limit = body_limit, .body_timeout = body_timeout, .idle_timeout = idle_timeout, .max_requests = max_requests, .proxy_config = proxy_config };
    int run_result = -1;
    sigset_t termination, previous_mask;
    sigemptyset(&termination);
    sigaddset(&termination, SIGINT);
    sigaddset(&termination, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &termination, &previous_mask))
        return -1;
    /* 슬롯을 재사용해도 세대 번호가 달라 늦은 epoll 이벤트가 새 연결에 적용되지 않는다. */
    if (allocate_loop_resources(&loop, limit) < 0)
        goto fail;
    if (pool_init(&loop.pool, workers, queue_limit, root, loop.wake) < 0)
        goto fail;
    struct epoll_event wake = { .events = EPOLLIN, .data.u64 = WAKE_KEY };
    if (epoll_ctl(loop.epoll, EPOLL_CTL_ADD, loop.wake, &wake) < 0)
        goto stop_pool;
    stop_requested = 0;
    signal_wake = loop.wake;
    struct sigaction action = { 0 };
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
        goto stop_pool;
    pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    run_result = 0;
    struct sockaddr_in bound;
    socklen_t bound_length = sizeof(bound);
    if (getsockname(listener, (struct sockaddr*)&bound, &bound_length) == 0) {
        printf("Listening on port %u\n", (unsigned)ntohs(bound.sin_port));
        fflush(stdout);
    }
    monitor_listener(&loop);
    run_event_loop(&loop);
stop_pool:
    signal_wake = -1;
    pthread_sigmask(SIG_BLOCK, &termination, NULL);
    stop_pool(&loop.pool);
    release_loop_resources(&loop);
    pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    return run_result;
fail:
    release_loop_resources(&loop);
    pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
    return -1;
}
