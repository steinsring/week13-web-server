#define _GNU_SOURCE
#include "concurrency.h"
#include "http_request.h"
#include "http_response.h"
#include "static_files.h"
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
#include <arpa/inet.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_SIZE 8192
#define LISTENER_KEY UINT64_MAX
#define WAKE_KEY (UINT64_MAX-1)
typedef struct Connection Connection;
typedef struct Job Job;
typedef enum { RECEIVING, WAITING, RETRYING, SENDING } Phase;
typedef enum { IDLE, QUEUED, RUNNING, DONE } JobState;
struct Job {
    Job *next;
    Connection *connection;
    JobState state;             /* pool.mutex로 보호한다. */
    int first, status, fd;
    off_t remaining, total;
    const char *mime;
    char location[HTTP_HEADER_LIMIT+4];
    char bytes[CHUNK_SIZE];
    size_t length;
    int64_t queued_at;
};
struct Connection {
    int socket, closed;
    uint32_t slot, generation;
    Phase phase;
    HttpRequest request;       /* 내부 문자열 포인터를 위해 객체를 이동하지 않는다. */
    Job job;                   /* 연결당 하나: 완료 결과 슬롯도 미리 확보된다. */
    char header[HTTP_HEADER_LIMIT+1024];
    size_t header_length, header_sent, chunk_sent;
    uint64_t bytes_sent;
    int64_t deadline;
};
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t *threads;
    size_t thread_count, queued, limit;
    int stopping, root, wake;
    Job *head, *tail, *done_head, *done_tail;
} Pool;
typedef struct {
    int epoll, listener, wake, listening, stopping;
    int64_t shutdown_at, accept_retry_at;
    size_t limit, objects;
    Connection **slots;
    uint32_t *generations;
    Pool pool;
} Loop;
static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t signal_wake = -1;

static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)<0) abort();
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void notify(int fd)
{
    uint64_t one=1;
    while(write(fd,&one,sizeof(one))<0 && errno==EINTR) {}
}
static void stop_signal(int sig)
{
    (void)sig; int saved=errno;
    stop_requested=1;
    if(signal_wake>=0) { uint64_t one=1; (void)write(signal_wake,&one,sizeof(one)); }
    errno=saved;
}

/* 이 함수에는 소켓 I/O가 없다. worker 하나가 파일 FD와 한 조각만 다룬다. */
static void execute_job(Pool *pool, Job *job)
{
    job->length=0; job->status=200;
    if(job->first) {
        StaticFileResult r;
        static_files_prepare(pool->root,&job->connection->request,&r);
        job->fd=r.fd; job->status=r.status; job->remaining=job->total=r.size;
        job->mime=r.mime;
        memcpy(job->location,r.location,sizeof(job->location));
        if(job->status!=200) return;
    }
    if(job->remaining==0) return;
    size_t want=job->remaining>(off_t)CHUNK_SIZE?CHUNK_SIZE:(size_t)job->remaining;
    ssize_t n;
    do { n=read(job->fd,job->bytes,want); } while(n<0 && errno==EINTR);
    if(n<=0) { job->status=500; return; }
    job->length=(size_t)n; job->remaining-=n;
}
static void *worker(void *argument)
{
    Pool *p=argument;
    for(;;) {
        pthread_mutex_lock(&p->mutex);
        while(!p->head && !p->stopping) pthread_cond_wait(&p->ready,&p->mutex);
        if(!p->head && p->stopping) { pthread_mutex_unlock(&p->mutex); return NULL; }
        Job *job=p->head; p->head=job->next;
        if(!p->head) p->tail=NULL;
        --p->queued;
        /* 인수 시각에 대기 기한을 검사한다. 실제 파일 실행 시간은 별개다. */
        int expired=now_ms()-job->queued_at>=10000;
        job->state=RUNNING;
        pthread_mutex_unlock(&p->mutex);
        notify(p->wake);        /* 대기열 빈자리도 루프에 알린다. */
        if(expired) { job->status=503; job->length=0; }
        else execute_job(p,job);
        pthread_mutex_lock(&p->mutex);
        job->state=DONE; job->next=NULL;
        if(p->done_tail) p->done_tail->next=job; else p->done_head=job;
        p->done_tail=job;
        pthread_mutex_unlock(&p->mutex);
        notify(p->wake);
    }
}
static int pool_init(Pool *p,size_t count,size_t limit,int root,int wake)
{
    memset(p,0,sizeof(*p)); p->limit=limit;p->root=root;p->wake=wake;
    if(pthread_mutex_init(&p->mutex,NULL)) return -1;
    if(pthread_cond_init(&p->ready,NULL)) {pthread_mutex_destroy(&p->mutex);return -1;}
    p->threads=calloc(count,sizeof(*p->threads));
    if(!p->threads) goto failure;
    for(;p->thread_count<count;++p->thread_count)
        if(pthread_create(&p->threads[p->thread_count],NULL,worker,p)) goto failure;
    return 0;
failure:
    pthread_mutex_lock(&p->mutex);p->stopping=1;pthread_cond_broadcast(&p->ready);pthread_mutex_unlock(&p->mutex);
    for(size_t i=0;i<p->thread_count;++i) pthread_join(p->threads[i],NULL);
    free(p->threads);pthread_cond_destroy(&p->ready);pthread_mutex_destroy(&p->mutex);return -1;
}
static JobState job_state(Pool *p,Job *j)
{
    pthread_mutex_lock(&p->mutex);JobState state=j->state;pthread_mutex_unlock(&p->mutex);return state;
}
static int submit(Pool *p,Job *j)
{
    int ok=0;
    pthread_mutex_lock(&p->mutex);
    if(p->queued<p->limit) {
        j->next=NULL;j->state=QUEUED;
        if(p->tail)p->tail->next=j;else p->head=j;
        p->tail=j;++p->queued;ok=1;pthread_cond_signal(&p->ready);
    }
    pthread_mutex_unlock(&p->mutex);return ok;
}
/* 아직 실행되지 않은 작업만 제거한다. RUNNING의 파일/요청은 만지지 않는다. */
static int cancel_queued(Pool *p,Job *j)
{
    int canceled=0;
    pthread_mutex_lock(&p->mutex);
    if(j->state==QUEUED) {
        Job **at=&p->head,*prev=NULL;
        while(*at && *at!=j) {prev=*at;at=&(*at)->next;}
        if(*at) { *at=j->next;if(p->tail==j)p->tail=prev;--p->queued;j->state=IDLE;canceled=1; }
    }
    pthread_mutex_unlock(&p->mutex);return canceled;
}
static uint64_t key(Connection *c) {return ((uint64_t)c->generation<<32)|c->slot;}
static int interest(Loop *l,Connection *c,uint32_t events)
{
    struct epoll_event e={.events=events | (events & EPOLLIN ? EPOLLRDHUP : 0),.data.u64=key(c)};
    return epoll_ctl(l->epoll,EPOLL_CTL_MOD,c->socket,&e);
}
static void release_connection(Loop *l,Connection *c)
{
    if(c->job.fd>=0)close(c->job.fd);
    http_request_destroy(&c->request);
    l->slots[c->slot]=NULL;--l->objects;free(c);
}
static void close_connection(Loop *l,Connection *c)
{
    if(c->closed)return;
    c->closed=1;epoll_ctl(l->epoll,EPOLL_CTL_DEL,c->socket,NULL);close(c->socket);
    JobState state=job_state(&l->pool,&c->job);
    if(state==QUEUED && cancel_queued(&l->pool,&c->job)) state=IDLE;
    else if(state==QUEUED)state=job_state(&l->pool,&c->job);
    /* DONE도 완료 큐에서 꺼낼 때까지 참조를 보존한다. */
    if(state==IDLE)release_connection(l,c);
}
static void error_response(Loop *l,Connection *c,int status)
{
    if(c->closed)return;
    if(c->bytes_sent){close_connection(l,c);return;}
    if(c->job.fd>=0){close(c->job.fd);c->job.fd=-1;}
    const char *body=http_status_reason(status);
    c->job.length=strlen(body);memcpy(c->job.bytes,body,c->job.length);
    c->job.remaining=0;
    int n=http_format_header(c->header,sizeof(c->header),status,"text/plain; charset=utf-8",(off_t)c->job.length,NULL);
    if(n<0){close_connection(l,c);return;}
    c->header_length=(size_t)n;c->header_sent=c->chunk_sent=0;
    c->phase=SENDING;c->deadline=now_ms()+10000;
    if(interest(l,c,EPOLLOUT)<0)close_connection(l,c);
}
static void schedule_read(Loop *l,Connection *c,int first)
{
    c->job.first=first;c->job.queued_at=now_ms();c->deadline=c->job.queued_at+10000;
    c->phase=WAITING;
    if(interest(l,c,0)<0){close_connection(l,c);return;}
    if(!submit(&l->pool,&c->job)) {
        if(first)error_response(l,c,503);else c->phase=RETRYING;
    }
}
static void retry_waiters(Loop *l)
{
    for(;;) {
        Connection *oldest=NULL;
        for(size_t i=0;i<l->limit;++i){Connection *c=l->slots[i];
            if(c&&!c->closed&&c->phase==RETRYING&&(!oldest||c->job.queued_at<oldest->job.queued_at))oldest=c;
        }
        if(!oldest)return;
        if(now_ms()>=oldest->deadline){error_response(l,oldest,503);continue;}
        if(!submit(&l->pool,&oldest->job))return;
        oldest->phase=WAITING;
    }
}
static void completed_jobs(Loop *l)
{
    Pool *p=&l->pool;
    pthread_mutex_lock(&p->mutex);Job *jobs=p->done_head;p->done_head=p->done_tail=NULL;pthread_mutex_unlock(&p->mutex);
    while(jobs){Job *j=jobs;jobs=j->next;Connection *c=j->connection;
        pthread_mutex_lock(&p->mutex);j->state=IDLE;pthread_mutex_unlock(&p->mutex);
        if(c->closed){release_connection(l,c);continue;}
        if(j->status!=200 && j->status!=301){error_response(l,c,j->status);continue;}
        c->chunk_sent=0;
        if(j->first){
            int n=http_format_header(c->header,sizeof(c->header),j->status,j->mime,j->total,j->status==301?j->location:NULL);
            if(n<0){error_response(l,c,500);continue;}
            c->header_length=(size_t)n;c->header_sent=0;
        }
        c->phase=SENDING;c->deadline=now_ms()+10000;
    if(interest(l,c,EPOLLOUT)<0)close_connection(l,c);
    }
    retry_waiters(l);
}
static int parse_status(HttpParseResult r)
{
    switch(r){case HTTP_PARSE_INVALID:return 400;case HTTP_PARSE_TOO_LARGE:return 431;
    case HTTP_PARSE_NO_MEMORY:return 500;case HTTP_PARSE_UNSUPPORTED_VERSION:return 505;default:return 500;}
}
static void receive_connection(Loop *l,Connection *c)
{
    char data[4096];
    for(;;){ssize_t n=recv(c->socket,data,sizeof(data),0);
        if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return;close_connection(l,c);return;}
        if(n==0){if(c->request.used)error_response(l,c,400);else close_connection(l,c);return;}
        if(now_ms()>=c->deadline){error_response(l,c,408);return;}
        HttpParseResult r=http_request_feed(&c->request,data,(size_t)n);
        if(r==HTTP_PARSE_INCOMPLETE)continue;
        if(r!=HTTP_PARSE_COMPLETE){error_response(l,c,parse_status(r));return;}
        if(strcmp(c->request.method,"GET")){error_response(l,c,501);return;}
        retry_waiters(l);schedule_read(l,c,1);return;
    }
}
static void send_connection(Loop *l,Connection *c)
{
    /* 이 연결의 현재 한 조각만 송신한다. 다음 파일 읽기는 모두 보낸 뒤 제출한다. */
    while(c->header_sent<c->header_length || c->chunk_sent<c->job.length){
        int header=c->header_sent<c->header_length;
        const char *data=header?c->header+c->header_sent:c->job.bytes+c->chunk_sent;
        size_t length=header?c->header_length-c->header_sent:c->job.length-c->chunk_sent;
        ssize_t n=send(c->socket,data,length,0);
        if(n<0){if(errno==EINTR)continue;if(errno==EAGAIN||errno==EWOULDBLOCK)return;close_connection(l,c);return;}
        if(n==0){close_connection(l,c);return;}
        c->bytes_sent+=(size_t)n;c->deadline=now_ms()+10000;
        if(header)c->header_sent+=(size_t)n;else c->chunk_sent+=(size_t)n;
    }
    if(c->job.remaining>0){retry_waiters(l);schedule_read(l,c,0);}else close_connection(l,c);
}
static void monitor_listener(Loop *l)
{
    int enabled=!l->stopping && l->objects<l->limit && now_ms()>=l->accept_retry_at;
    if(enabled==l->listening)return;
    struct epoll_event e={.events=EPOLLIN,.data.u64=LISTENER_KEY};
    if(epoll_ctl(l->epoll,enabled?EPOLL_CTL_ADD:EPOLL_CTL_DEL,l->listener,&e)==0)l->listening=enabled;
}
static void accept_connections(Loop *l)
{
    while(!l->stopping&&l->objects<l->limit){
        int fd=accept4(l->listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
        if(fd<0){
            if(errno==EINTR)continue;
            if(errno!=EAGAIN&&errno!=EWOULDBLOCK){perror("accept");l->accept_retry_at=now_ms()+1000;}
            break;
        }
        size_t slot=0;while(l->slots[slot])++slot;
        Connection *c=calloc(1,sizeof(*c));
        if(!c){close(fd);l->accept_retry_at=now_ms()+1000;break;}
        c->socket=fd;c->slot=(uint32_t)slot;c->generation=++l->generations[slot];
        c->job.fd=-1;c->job.connection=c;c->phase=RECEIVING;c->deadline=now_ms()+10000;
        http_request_init(&c->request);l->slots[slot]=c;++l->objects;
        struct epoll_event e={.events=EPOLLIN|EPOLLRDHUP,.data.u64=key(c)};
        if(epoll_ctl(l->epoll,EPOLL_CTL_ADD,fd,&e)<0)close_connection(l,c);
    }
    monitor_listener(l);
}
static int next_timeout(Loop *l)
{
    int64_t now=now_ms(),nearest=now+1000;
    if(l->stopping&&l->shutdown_at<nearest)nearest=l->shutdown_at;
    if(l->accept_retry_at>now&&l->accept_retry_at<nearest)nearest=l->accept_retry_at;
    for(size_t i=0;i<l->limit;++i){Connection *c=l->slots[i];if(!c||c->closed)continue;
        if(c->phase==WAITING&&job_state(&l->pool,&c->job)!=QUEUED)continue;
        if(c->deadline<nearest)nearest=c->deadline;
    }
    return nearest<=now?0:(int)(nearest-now);
}
static void expire_connections(Loop *l)
{
    int64_t now=now_ms();
    for(size_t i=0;i<l->limit;++i){Connection *c=l->slots[i];if(!c||c->closed||now<c->deadline)continue;
        if(c->phase==RECEIVING)error_response(l,c,408);
        else if(c->phase==SENDING)close_connection(l,c);
        else if(c->phase==RETRYING)error_response(l,c,503);
        else if(cancel_queued(&l->pool,&c->job))error_response(l,c,503);
    }
}
int concurrent_server_run(int listener,int root,size_t workers,size_t queue_limit,size_t limit)
{
    Loop l={.listener=listener};
    int run_result=-1;
    sigset_t termination, previous_mask;
    sigemptyset(&termination);sigaddset(&termination,SIGINT);sigaddset(&termination,SIGTERM);
    if(pthread_sigmask(SIG_BLOCK,&termination,&previous_mask))return -1;
    /* 슬롯을 재사용해도 세대 번호가 달라 늦은 epoll 이벤트가 새 연결에 적용되지 않는다. */
    l.limit=limit;l.epoll=-1;l.wake=-1;
    l.slots=calloc(limit,sizeof(*l.slots));l.generations=calloc(limit,sizeof(*l.generations));
    if(!l.slots||!l.generations)goto fail;
    l.epoll=epoll_create1(EPOLL_CLOEXEC);l.wake=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
    if(l.epoll<0||l.wake<0)goto fail;
    if(pool_init(&l.pool,workers,queue_limit,root,l.wake)<0)goto fail;
    struct epoll_event wake={.events=EPOLLIN,.data.u64=WAKE_KEY};
    if(epoll_ctl(l.epoll,EPOLL_CTL_ADD,l.wake,&wake)<0)goto stop_pool;
    stop_requested=0;signal_wake=l.wake;
    struct sigaction action={0};action.sa_handler=stop_signal;sigemptyset(&action.sa_mask);
    if(sigaction(SIGINT,&action,NULL)||sigaction(SIGTERM,&action,NULL))goto stop_pool;
    pthread_sigmask(SIG_SETMASK,&previous_mask,NULL);
    run_result=0;
    struct sockaddr_in bound; socklen_t bound_length=sizeof(bound);
    if(getsockname(listener,(struct sockaddr *)&bound,&bound_length)==0) {
        printf("Listening on port %u\n",(unsigned)ntohs(bound.sin_port));fflush(stdout);
    }
    monitor_listener(&l);
    for(;;){
        if(stop_requested&&!l.stopping){l.stopping=1;l.shutdown_at=now_ms()+10000;monitor_listener(&l);}
        completed_jobs(&l);expire_connections(&l);retry_waiters(&l);monitor_listener(&l);
        if(l.stopping && l.objects==0)break;
        if(l.stopping&&now_ms()>=l.shutdown_at){
            /* 실제 파일 I/O는 강제로 취소하지 않는다. 기한 종료는 프로세스 전체를 끝낸다. */
            for(size_t i=0;i<limit;++i)if(l.slots[i]&&!l.slots[i]->closed)close(l.slots[i]->socket);
            _exit(EXIT_SUCCESS);
        }
        struct epoll_event events[64];
        int n=epoll_wait(l.epoll,events,64,next_timeout(&l));
        if(n<0){if(errno==EINTR)continue;perror("epoll_wait");l.stopping=1;l.shutdown_at=now_ms();continue;}
        for(int i=0;i<n;++i){uint64_t id=events[i].data.u64;
            if(id==WAKE_KEY){uint64_t value;while(read(l.wake,&value,sizeof(value))>0){};continue;}
            if(id==LISTENER_KEY){if(!stop_requested)accept_connections(&l);continue;}
            uint32_t slot=(uint32_t)id,gen=(uint32_t)(id>>32);
            if(slot>=limit)continue;
            Connection *c=l.slots[slot];if(!c||c->generation!=gen||c->closed)continue;
            uint32_t e=events[i].events;
            if(e&(EPOLLERR|EPOLLHUP)){close_connection(&l,c);continue;}
            if(c->phase==RECEIVING&&(e&(EPOLLIN|EPOLLRDHUP)))receive_connection(&l,c);
            else if(c->phase==SENDING&&(e&EPOLLOUT))send_connection(&l,c);
            /* 대기 중 RDHUP는 상대의 write-half close일 수 있어 송신 가능성을 유지한다. */
        }
    }
stop_pool:
    signal_wake=-1;
    pthread_sigmask(SIG_BLOCK,&termination,NULL);
    pthread_mutex_lock(&l.pool.mutex);l.pool.stopping=1;pthread_cond_broadcast(&l.pool.ready);pthread_mutex_unlock(&l.pool.mutex);
    for(size_t i=0;i<l.pool.thread_count;++i)pthread_join(l.pool.threads[i],NULL);
    free(l.pool.threads);pthread_cond_destroy(&l.pool.ready);pthread_mutex_destroy(&l.pool.mutex);
    close(l.wake);close(l.epoll);free(l.slots);free(l.generations);pthread_sigmask(SIG_SETMASK,&previous_mask,NULL);return run_result;
fail:
    if(l.wake>=0)close(l.wake);
    if(l.epoll>=0)close(l.epoll);
    free(l.slots);free(l.generations);pthread_sigmask(SIG_SETMASK,&previous_mask,NULL);return -1;
}
