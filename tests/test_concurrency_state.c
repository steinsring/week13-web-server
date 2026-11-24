/* 내부 상태 전이 검증: 실제 파일 지연 없이 큐 포화와 후속 읽기 재시도를 재현한다. */
#define _GNU_SOURCE
#include <sys/socket.h>
static ssize_t partial_send(int fd, const void *data, size_t size, int flags);
#define send partial_send
#include "../concurrency.c"
#undef send
#include <assert.h>
static int partial_mode, send_step;
static ssize_t partial_send(int fd, const void *data, size_t size, int flags)
{
    if (partial_mode) {
        ++send_step;
        if (send_step == 1) { errno = EINTR; return -1; }
        if (send_step == 3) { errno = EAGAIN; return -1; }
        if (size > 3) size = 3;
    }
    return send(fd, data, size, flags);
}

static Connection *connection(Loop *l,int *peer)
{
    int pair[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    Connection *c=calloc(1,sizeof(*c));assert(c);
    c->socket=pair[0];*peer=pair[1];c->slot=0;c->generation=++l->generations[0];
    c->job.connection=c;c->job.fd=-1;l->slots[0]=c;++l->objects;
    struct epoll_event e={.events=EPOLLIN,.data.u64=key(c)};
    assert(epoll_ctl(l->epoll,EPOLL_CTL_ADD,c->socket,&e)==0);return c;
}
int main(void)
{
    Loop l={0};l.limit=1;l.slots=calloc(1,sizeof(*l.slots));l.generations=calloc(1,sizeof(*l.generations));
    l.epoll=epoll_create1(0);assert(l.epoll>=0);
    pthread_mutex_init(&l.pool.mutex,NULL);pthread_cond_init(&l.pool.ready,NULL);l.pool.limit=1;
    Job blocking={.fd=-1};blocking.queued_at=now_ms();assert(submit(&l.pool,&blocking));
    int peer;Connection *c=connection(&l,&peer);
    schedule_read(&l,c,1);assert(c->phase==SENDING);assert(strstr(c->header,"503 Service Unavailable"));
    assert(l.pool.queued==1);close_connection(&l,c);close(peer);
    c=connection(&l,&peer);schedule_read(&l,c,0);assert(c->phase==RETRYING);
    int64_t started=c->job.queued_at;retry_waiters(&l);assert(c->phase==RETRYING&&c->job.queued_at==started);
    assert(cancel_queued(&l.pool,&blocking));retry_waiters(&l);
    assert(c->phase==WAITING&&job_state(&l.pool,&c->job)==QUEUED&&c->job.queued_at==started);
    close_connection(&l,c);close(peer);assert(l.pool.queued==0&&l.objects==0);
    c=connection(&l,&peer);c->phase=RETRYING;c->bytes_sent=1;c->deadline=now_ms()-1;
    expire_connections(&l);assert(l.objects==0);close(peer);
    c=connection(&l,&peer);schedule_read(&l,c,0);c->deadline=now_ms()-1;
    expire_connections(&l);assert(c->phase==SENDING&&strstr(c->header,"503 Service Unavailable"));
    assert(l.pool.queued==0);close_connection(&l,c);close(peer);
    c=connection(&l,&peer);c->phase=WAITING;c->deadline=now_ms()-1;c->job.state=RUNNING;
    expire_connections(&l);assert(!c->closed&&c->phase==WAITING);
    close_connection(&l,c);assert(c->closed&&l.objects==1);
    c->job.state=DONE;l.pool.done_head=l.pool.done_tail=&c->job;
    completed_jobs(&l);assert(l.objects==0&&l.slots[0]==NULL);close(peer);
    /* 일부 100 → EAGAIN → 최종 400: 응답 사이의 바이트 순서를 보존한다. */
    c=connection(&l,&peer);c->phase=BODY_RECEIVING;
    c->interim_pending=1;c->interim_deadline=now_ms()+10000;
    c->body_deadline=now_ms()+10000;
    partial_mode=1;send_step=0;
    assert(send_continue(&l,c)==0 && c->interim_sent==3 && c->bytes_sent==0);
    error_response(&l,c,400);
    assert(c->interim_pending && c->bytes_sent==0);
    send_connection(&l,c);assert(l.objects==0);
    char received[512];ssize_t count=recv(peer,received,sizeof(received)-1,0);assert(count>0);
    received[count]=0;
    assert(strstr(received,"HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 400 Bad Request\r\n")==received);
    assert(strstr(received,"\r\n\r\nBad Request"));close(peer);
    partial_mode=0;
    c=connection(&l,&peer);c->phase=BODY_RECEIVING;c->interim_pending=1;
    error_response(&l,c,408);assert(!c->interim_pending);
    close_connection(&l,c);close(peer);
    close(l.epoll);pthread_cond_destroy(&l.pool.ready);pthread_mutex_destroy(&l.pool.mutex);
    free(l.slots);free(l.generations);
    puts("PASS: queue/retry/deadline/lifetime and partial 100/EINTR/EAGAIN before final error");
}
