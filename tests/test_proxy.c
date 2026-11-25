/* 내부 파서에 모든 분할 위치를 공급해 TCP 패킷 경계에 의존하지 않음을 검사한다. */
#include <stddef.h>
#include <stdlib.h>
static void *test_proxy_calloc(size_t count, size_t size);
#define calloc test_proxy_calloc
#include "../proxy.c"
#undef calloc
#include <assert.h>
static int fail_calloc;
static void *test_proxy_calloc(size_t count, size_t size)
{
    return fail_calloc ? NULL : calloc(count,size);
}

static int wire_result(const char *wire, size_t length, size_t split,
                       char *decoded, size_t *decoded_length)
{
    ProxyConfig config={0};strcpy(config.public_origin.text,"https://public.example");
    Proxy *p=calloc(1,sizeof(*p));assert(p);p->fd=-1;p->config=&config;
    p->phase=RESPONSE_READING;p->deadline=10000;
    size_t parts[]={0,split,length};int status=0;
    *decoded_length=0;
    for(unsigned i=0;i<2 && !status && p->phase!=FINISHED;++i) {
        size_t n=parts[i+1]-parts[i];assert(n<=sizeof(p->input));
        memcpy(p->input,wire+parts[i],n);p->input_length=n;p->input_position=0;
        do {
            status=pump_input(p);
            if(status)break;
            if(p->output_length) {
                if(!p->output_header) {
                    const char *body=p->output;size_t bytes=p->output_length;
                    if(p->encode_chunks) {
                        bytes=(size_t)strtoull(body,NULL,16);body=strstr(body,"\r\n")+2;
                    }
                    memcpy(decoded+*decoded_length,body,bytes);*decoded_length+=bytes;
                }
                proxy_consume_output(p,p->output_length,0);
            }
        } while(p->input_position<p->input_length && p->phase!=FINISHED);
    }
    proxy_destroy(p);return status;
}
int main(void)
{
    assert(proxy_route("/api")&&proxy_route("/api/x")&&!proxy_route("/apix")&&!proxy_route("/%61pi"));
    const char wire[]="HTTP/1.1 103 Early Hints\r\nLink: </x>\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=\"x,y\"\r\na\0\nb\r\n3\r\nxyz\r\n0\r\nX-End: yes\r\n\r\n";
    for(size_t split=0;split<sizeof(wire);++split) {
        char decoded[512];size_t n;
        assert(!wire_result(wire,sizeof(wire)-1,split,decoded,&n));
        assert(n==7&&!memcmp(decoded,"a\0\nbxyz",7));
    }
    const char *bad[]={
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551616\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX: a\001b\r\n\r\n",
        "HTTP/1.1 101 Upgrade\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx!\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nContent-Length: 1\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1;x=\"bad\r\n",
    };
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        for(size_t split=0;split<=strlen(bad[i]);++split) {
            char decoded[512];size_t n;
            assert(wire_result(bad[i],strlen(bad[i]),split,decoded,&n)==502);
        }
    }
    /* 의도적 수신 정지는 새 제한 시간을 주지 않고 남은 60ms만 보존한다. */
    Proxy p={0};p.fd=-1;p.phase=RESPONSE_READING;p.deadline=100;p.output_length=1;
    pause_reading(&p,40);assert(proxy_deadline(&p)==INT64_MAX&&p.paused_remaining==60);
    proxy_consume_output(&p,1,1000);assert(p.deadline==1060);
    p.output_length=1;pause_reading(&p,1020);assert(p.paused_remaining==40);
    proxy_consume_output(&p,1,2000);assert(p.deadline==2040);
    p.phase=CONNECTING;p.deadline=2000;assert(proxy_step(&p,0,2000)==504);
    p.phase=REQUEST_SENDING;assert(proxy_step(&p,0,2000)==504);

    ProxyConfig config;assert(!proxy_config_init(&config,"https://public.example:8443","http://localhost:80,http://[::1]:8001",5,10));
    char location[1024];
    assert(!proxy_rewrite_location(&config,"http://LOCALHOST/api?q=1#f",location,sizeof(location))&&!strcmp(location,"https://public.example:8443/api?q=1#f"));
    assert(!proxy_rewrite_location(&config,"//[::1]:8001/api",location,sizeof(location))&&!strcmp(location,"https://public.example:8443/api"));
    assert(!proxy_rewrite_location(&config,"http://localhost.evil/api",location,sizeof(location))&&!strcmp(location,"http://localhost.evil/api"));
    assert(proxy_rewrite_location(&config,"http:/bad",location,sizeof(location))<0);
    assert(proxy_rewrite_location(&config,"/bad%xx",location,sizeof(location))<0);
    proxy_config_destroy(&config);
    HttpRequest request={0};HttpBody body={0};int status;
    fail_calloc=1;
    assert(!proxy_start(&config,&request,&body,0,0,&status)&&status==500);
    fail_calloc=0;
    assert(proxy_config_init(&config,"http://x","http://localhost",0,1)<0);
    puts("PASS: proxy every-split chunk/header validation, timer preservation, origins/IPv6 and Location boundaries");
}
