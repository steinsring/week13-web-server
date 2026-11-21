#define _POSIX_C_SOURCE 200809L
#include "static_files.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void check(int root, const char *path, const char *query, int status)
{
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    HttpRequest request;
    http_request_init(&request);
    request.path=(char *)path; request.query=(char *)query;
    assert(static_files_serve(pair[0],root,&request)==0);
    close(pair[0]);
    char result[1024]; size_t used=0; ssize_t count;
    while((count=read(pair[1],result+used,sizeof(result)-1-used))>0) used+=(size_t)count;
    assert(count==0); result[used]=0;
    char expected[32]; snprintf(expected,sizeof(expected),"HTTP/1.1 %d ",status);
    assert(strncmp(result,expected,strlen(expected))==0);
    close(pair[1]); http_request_destroy(&request);
}

int main(void)
{
    char path[]="/tmp/week13-static-unit-XXXXXX";
    assert(mkdtemp(path));
    int root=static_root_open(path); assert(root>=0);
    int file=openat(root,"index.html",O_WRONLY|O_CREAT,0600);
    assert(file>=0); assert(write(file,"hello",5)==5); close(file);
    assert(mkdirat(root,"docs",0700)==0);
    assert(symlinkat("index.html",root,"link")==0);
    for(int i=0;i<300;++i){
        check(root,"/",NULL,200);
        check(root,"/missing",NULL,404);
        check(root,"/docs","q=one",301);
        check(root,"/../secret",NULL,403);
        check(root,"/link",NULL,403);
        check(root,"/%GG",NULL,400);
        check(root,"/%00",NULL,400);
        check(root,"/docs/",NULL,404);
    }
    assert(strcmp(static_mime_type("x.CSS"),"text/css; charset=utf-8")==0);
    assert(strcmp(static_mime_type("x.unknown"),"application/octet-stream")==0);
    assert(unlinkat(root,"index.html",0)==0); assert(unlinkat(root,"link",0)==0);
    assert(unlinkat(root,"docs",AT_REMOVEDIR)==0); close(root); assert(rmdir(path)==0);
    puts("PASS: 2400 static response/cleanup checks and MIME mapping");
    return 0;
}
