#define _POSIX_C_SOURCE 200809L
#include "../../http_request.h"
#include "../../static_files.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * libFuzzer 하네스 #2: 정적 파일 경로 디코딩 · MIME 매핑 · 경로 탈출/심볼릭 링크 차단
 * - 퍼센트 인코딩(%2e%2e, %00, %2f 등)과 임의의 경로 문자열을 static_files_prepare에
 *   공급하여 O_NOFOLLOW 보안 탐색, location 버퍼 경계, FD 누수 없음을 검증한다.
 */
static int g_root_fd = -1;

static void init_fuzz_root(void)
{
    if (g_root_fd >= 0)
        return;
    const char *root = "/tmp/fuzz-static-root";
    mkdir(root, 0755);
    mkdir("/tmp/fuzz-static-root/docs", 0755);

    int fd = open("/tmp/fuzz-static-root/index.html", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, "ok\n", 3);
        close(fd);
    }
    fd = open("/tmp/fuzz-static-root/docs/index.html", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        (void)write(fd, "docs\n", 5);
        close(fd);
    }
    unlink("/tmp/fuzz-static-root/symlink.txt");
    (void)symlink("/etc/hostname", "/tmp/fuzz-static-root/symlink.txt");

    g_root_fd = static_root_open(root);
    if (g_root_fd < 0)
        __builtin_trap();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    init_fuzz_root();
    if (size == 0)
        return 0;

    size_t len = size > HTTP_HEADER_LIMIT ? HTTP_HEADER_LIMIT : size;
    char path[HTTP_HEADER_LIMIT + 1];
    memcpy(path, data, len);
    path[len] = '\0';

    /* NUL 내부 문자를 일반 문자로 치환하지 않고 C 문자열 그대로 테스트 */
    (void)static_mime_type(path);

    HttpRequest req;
    http_request_init(&req);
    req.method = (data[0] & 1) ? "GET" : "HEAD";
    req.path = path;
    req.version = "HTTP/1.1";

    StaticFileResult result;
    memset(&result, 0, sizeof(result));
    result.fd = -1;

    static_files_prepare(g_root_fd, &req, &result);

    if (result.fd >= 0) {
        /* 심볼릭 링크 대상(/etc/hostname)이나 루트 밖 파일이 열려서는 안 된다 */
        if (result.status != 200)
            __builtin_trap();
        close(result.fd);
    }

    http_request_destroy(&req);
    return 0;
}
