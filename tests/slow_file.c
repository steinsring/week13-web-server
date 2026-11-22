#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int __real_openat(int,const char *,int,...);
int __wrap_openat(int fd,const char *path,int flags,...)
{
    mode_t mode=0;
    if(flags&O_CREAT){va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
    if(!strcmp(path,"slow.txt")) {
        const char *value=getenv("TEST_OPEN_DELAY_MS");
        long ms=value?strtol(value,NULL,10):0;
        struct timespec delay={ms/1000,(ms%1000)*1000000};
        while(nanosleep(&delay,&delay)<0){}
    }
    return __real_openat(fd,path,flags,mode);
}
