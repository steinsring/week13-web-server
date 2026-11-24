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
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_SIZE 8192
#define LISTENER_KEY UINT64_MAX
#define WAKE_KEY (UINT64_MAX - 1)
typedef struct Connection Connection;
typedef struct Job Job;
typedef enum { RECEIVING,
    BODY_RECEIVING,
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
    Connection** slots;
    uint32_t* generations;
    Pool pool;
} Loop;
static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t signal_wake = -1;

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
static void error_response(Loop* loop, Connection* connection, int status)
{
    if (connection->closed)
        return;
    if (connection->bytes_sent) {
        close_connection(loop, connection);
        return;
    }
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
    int n = http_format_header(connection->header, sizeof(connection->header), status, "text/plain; charset=utf-8", (off_t)connection->job.length, NULL);
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
        if (job->status != 200 && job->status != 301) {
            error_response(loop, connection, job->status);
            continue;
        }
        connection->chunk_sent = 0;
        if (job->first) {
            int n = http_format_header(connection->header, sizeof(connection->header), job->status, job->mime, job->total, job->status == 301 ? job->location : NULL);
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
/* POST는 파일 작업 없이 최종 응답만 준비한다. 본문은 연결 정리까지 보관한다. */
static void complete_post(Loop* loop, Connection* connection)
{
    int n = http_format_no_content(connection->header, sizeof(connection->header));
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
    if (!strcmp(connection->request.method, "GET")) {
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
    if (body_length == 0) {
        complete_post(loop, connection);
        return;
    }
    if (!receive_body_bytes(loop, connection, data, length))
        return;
    if (expect_continue) {
        connection->interim_pending = 1;
        connection->interim_deadline = now_ms() + 10000;
    }
    if (interest(loop, connection, EPOLLIN | (connection->interim_pending ? EPOLLOUT : 0)) < 0)
        close_connection(loop, connection);
}

static void receive_connection(Loop* loop, Connection* connection)
{
    char data[4096];
    /* LT epoll은 읽을 바이트가 남으면 다시 알려준다. 한 연결의 수신량을 제한한다. */
    for (unsigned turn = 0; turn < 16; ++turn) {
        int64_t deadline = connection->phase == BODY_RECEIVING ? connection->body_deadline : connection->deadline;
        if (now_ms() >= deadline) {
            error_response(loop, connection, 408);
            return;
        }
        ssize_t n = recv(connection->socket, data, sizeof(data), 0);
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
        if (connection->phase == BODY_RECEIVING) {
            if (!receive_body_bytes(loop, connection, data, (size_t)n))
                return;
            continue;
        }
        if (now_ms() >= connection->deadline) {
            error_response(loop, connection, 408);
            return;
        }
        size_t consumed;
        HttpParseResult result = http_request_feed_bytes(&connection->request, data, (size_t)n, &consumed);
        if (result == HTTP_PARSE_INCOMPLETE)
            continue;
        if (result != HTTP_PARSE_COMPLETE) {
            error_response(loop, connection, parse_status(result));
            return;
        }
        finish_request_headers(loop, connection, data + consumed, (size_t)n - consumed);
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

static void send_connection(Loop* loop, Connection* connection)
{
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
        close_connection(loop, connection);
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
    connection->generation = ++loop->generations[slot];
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
        if (connection->phase == SENDING && connection->interim_pending)
            continue;
        if (now < connection->deadline)
            continue;
        if (connection->phase == RECEIVING)
            error_response(loop, connection, 408);
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
    uint32_t slot = (uint32_t)id, gen = (uint32_t)(id >> 32);
    if (slot >= loop->limit)
        return;
    Connection* connection = loop->slots[slot];
    if (!connection || connection->generation != gen || connection->closed)
        return;
    uint32_t e = event->events;
    if (e & (EPOLLERR | EPOLLHUP)) {
        close_connection(loop, connection);
        return;
    }
    if ((connection->phase == RECEIVING || connection->phase == BODY_RECEIVING) && (e & (EPOLLIN | EPOLLRDHUP)))
        receive_connection(loop, connection);
    else if (connection->phase == BODY_RECEIVING && (e & EPOLLOUT))
        send_body_continue(loop, connection);
    else if (connection->phase == SENDING && (e & EPOLLOUT))
        send_connection(loop, connection);
    /* 대기 중 RDHUP는 상대의 write-half close일 수 있어 송신 가능성을 유지한다. */
}

static void force_shutdown(Loop* loop)
{
    /* 실행 중인 파일 I/O를 강제로 취소하지 않고 프로세스 전체를 종료한다. */
    for (size_t i = 0; i < loop->limit; ++i)
        if (loop->slots[i] && !loop->slots[i]->closed)
            close(loop->slots[i]->socket);
    _exit(EXIT_SUCCESS);
}

static void update_loop_state(Loop* loop)
{
    if (stop_requested && !loop->stopping) {
        loop->stopping = 1;
        loop->shutdown_at = now_ms() + 10000;
        monitor_listener(loop);
    }
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
    loop->limit = limit;
    loop->epoll = -1;
    loop->wake = -1;
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

int concurrent_server_run(int listener, int root, size_t workers, size_t queue_limit, size_t limit, size_t body_limit, size_t body_timeout)
{
    Loop loop = { .listener = listener, .body_limit = body_limit, .body_timeout = body_timeout };
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
