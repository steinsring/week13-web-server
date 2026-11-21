#define _POSIX_C_SOURCE 200809L
#include "static_files.h"
#include "http_response.h"
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int error_status(int error)
{
    if (error == ENOENT || error == ENOTDIR) return 404;
    if (error == EACCES || error == EPERM || error == ELOOP) return 403;
    return 500;
}

/* 링크는 열기 전과 실제 open 모두에서 거절한다. FIFO 열기도 대기하지 않는다. */
static int open_child(int parent, const char *name, int *status)
{
    struct stat before, after;
    if (fstatat(parent, name, &before, AT_SYMLINK_NOFOLLOW) < 0) {
        *status = error_status(errno); return -1;
    }
    if (!S_ISDIR(before.st_mode) && !S_ISREG(before.st_mode)) {
        *status = 403; return -1;
    }
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { *status = error_status(errno); return -1; }
    if (fstat(fd, &after) < 0) { close(fd); *status = 500; return -1; }
    if ((!S_ISDIR(after.st_mode) && !S_ISREG(after.st_mode)) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
        close(fd); *status = 403; return -1;
    }
    return fd;
}

/* 디렉터리 FD 스택으로 실제 경로를 순서대로 확인한다.
 * a/../b에서도 a를 건너뛰지 않는다. 루트 위로 올라갈 수 없다.
 */
static int walk(int root, char *path, int *status, int configuring_root)
{
    size_t capacity = strlen(path) + 2, depth = 0;
    int *stack = malloc(capacity * sizeof(*stack));
    if (!stack) { *status = 500; return -1; }
    stack[0] = dup(root);
    if (stack[0] < 0) { free(stack); *status = 500; return -1; }
    char *save;
    for (char *part = strtok_r(path, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        struct stat current;
        if (fstat(stack[depth], &current) < 0) { *status = 500; goto fail; }
        if (!S_ISDIR(current.st_mode)) { *status = 404; goto fail; }
        if (strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) {
            if (!depth) {
                if (!configuring_root) { *status = 403; goto fail; }
                int parent = open_child(stack[0], "..", status);
                if (parent < 0) goto fail;
                close(stack[0]); stack[0] = parent; continue;
            }
            close(stack[depth--]); continue;
        }
        int next = open_child(stack[depth], part, status);
        if (next < 0) goto fail;
        stack[++depth] = next;
    }
    int result = stack[depth];
    while (depth) close(stack[--depth]);
    free(stack);
    return result;
fail:
    do { close(stack[depth]); } while (depth--);
    free(stack);
    return -1;
}

int static_root_open(const char *path)
{
    if (!path || !*path) { errno = EINVAL; return -1; }
    int base = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (base < 0) return -1;
    char *copy = strdup(path);
    if (!copy) { close(base); return -1; }
    int status = 500;
    int fd = walk(base, copy, &status, 1);
    free(copy); close(base);
    struct stat st;
    if (fd >= 0 && (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) ||
                   faccessat(fd, ".", R_OK | X_OK, 0) < 0)) {
        close(fd); fd = -1;
    }
    if (fd < 0) errno = status == 403 ? EACCES : status == 404 ? ENOENT : EIO;
    return fd;
}

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *decode_path(const char *raw, int *status)
{
    if (!raw || raw[0] != '/') { *status = 400; return NULL; }
    char *decoded = malloc(strlen(raw) + 1);
    if (!decoded) { *status = 500; return NULL; }
    size_t out = 0;
    for (size_t i = 0; raw[i]; ++i) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '%') {
            if (!raw[i+1] || !raw[i+2] || hex(raw[i+1]) < 0 || hex(raw[i+2]) < 0) {
                free(decoded); *status = 400; return NULL;
            }
            c = (unsigned char)(hex(raw[i+1]) * 16 + hex(raw[i+2])); i += 2;
        }
        if (c < 0x20 || c == 0x7f) { free(decoded); *status = 400; return NULL; }
        decoded[out++] = (char)c;
    }
    decoded[out] = '\0';
    return decoded;
}

const char *static_mime_type(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return "application/octet-stream";
    ++ext;
    static const struct { const char *ext, *mime; } types[] = {
        {"html","text/html; charset=utf-8"}, {"htm","text/html; charset=utf-8"},
        {"css","text/css; charset=utf-8"}, {"js","text/javascript; charset=utf-8"},
        {"txt","text/plain; charset=utf-8"}, {"png","image/png"},
        {"jpg","image/jpeg"}, {"jpeg","image/jpeg"}, {"gif","image/gif"},
        {"svg","image/svg+xml"}, {"ico","image/vnd.microsoft.icon"}
    };
    for (size_t i=0; i<sizeof(types)/sizeof(types[0]); ++i)
        if (!strcasecmp(ext, types[i].ext)) return types[i].mime;
    return "application/octet-stream";
}

int static_files_serve(int client, int root, const HttpRequest *request)
{
    int status = 500, fd = -1, result;
    char *decoded = decode_path(request->path, &status);
    if (!decoded) return http_send_status(client, status);
    size_t length = strlen(decoded);
    int trailing_slash = length && decoded[length-1] == '/';
    const char *mime = static_mime_type(decoded);
    char *copy = strdup(decoded);
    if (!copy) { free(decoded); return http_send_status(client, 500); }
    fd = walk(root, copy, &status, 0);
    free(copy);
    if (fd < 0) { free(decoded); return http_send_status(client, status); }
    struct stat st;
    if (fstat(fd, &st) < 0) { status = 500; goto error; }
    if (S_ISDIR(st.st_mode)) {
        if (!trailing_slash) {
            /* Location은 디코딩 전 경로를 사용한다. //로 시작하는 외부 이동은 막는다. */
            const char *raw = request->path;
            while (raw[0] == '/' && raw[1] == '/') ++raw;
            if (strchr(raw, '\\')) { status = 400; goto error; }
            size_t size = strlen(raw) + (request->query ? strlen(request->query) : 0) + 4;
            char *location = malloc(size);
            if (!location) { status = 500; goto error; }
            snprintf(location, size, "%s/%s%s", raw, request->query ? "?" : "",
                     request->query ? request->query : "");
            result = http_send_redirect(client, location);
            free(location); close(fd); free(decoded); return result;
        }
        int index = open_child(fd, "index.html", &status);
        close(fd); fd = index;
        if (fd < 0) goto error;
        if (fstat(fd, &st) < 0) { status = 500; goto error; }
        if (!S_ISREG(st.st_mode)) { status = 403; goto error; }
        mime = static_mime_type("index.html");
    } else if (trailing_slash) { status = 404; goto error; }
    if (!S_ISREG(st.st_mode)) { status = 403; goto error; }
    result = http_send_file(client, fd, st.st_size, mime);
    close(fd); free(decoded); return result;
error:
    if (fd >= 0) close(fd);
    free(decoded);
    return http_send_status(client, status);
}
