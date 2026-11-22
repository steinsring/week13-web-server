#ifndef STATIC_FILES_H
#define STATIC_FILES_H
#include "http_request.h"
#include <sys/types.h>
int static_root_open(const char *path);
int static_files_serve(int client, int root, const HttpRequest *request);
const char *static_mime_type(const char *name);
typedef struct {
    int status, fd;
    off_t size;
    const char *mime;
    char location[HTTP_HEADER_LIMIT + 4];
} StaticFileResult;
/* 파일 작업만 수행한다. 소켓에는 접근하지 않는다. fd는 호출자가 소유한다. */
void static_files_prepare(int root, const HttpRequest *request, StaticFileResult *result);
#endif
