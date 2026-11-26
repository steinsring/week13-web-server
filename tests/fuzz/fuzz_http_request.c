#include "../../http_body.h"
#include "../../http_request.h"
#include "../../proxy.h"

#include <stddef.h>
#include <stdint.h>

/*
 * libFuzzer 하네스 #1: HTTP/1.1 요청 파서 + POST 본문 검증 + /api 라우팅
 * - 무작위 바이트를 임의의 TCP 조각 크기로 분할 공급하여 버퍼 경계·제어 문자·
 *   중복 헤더·Host 파싱·Content-Length 오버플로 등에서의 OOB/UB/누수를 검증한다.
 */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2)
        return 0;

    size_t step = (size_t)(data[0] % 64) + 1;
    const char *wire = (const char *)(data + 1);
    size_t remaining = size - 1;

    HttpRequest req;
    http_request_init(&req);

    size_t offset = 0;
    HttpParseResult res = HTTP_PARSE_INCOMPLETE;
    while (offset < remaining && res == HTTP_PARSE_INCOMPLETE) {
        size_t chunk = remaining - offset;
        if (chunk > step)
            chunk = step;
        size_t consumed = 0;
        res = http_request_feed_bytes(&req, wire + offset, chunk, &consumed);
        if (consumed > chunk)
            __builtin_trap();
        offset += consumed;
        if (res == HTTP_PARSE_INCOMPLETE && consumed == 0)
            break;
    }

    if (res == HTTP_PARSE_COMPLETE) {
        if (!req.method || !req.path || !req.version)
            __builtin_trap();
        (void)proxy_route(req.path);
        size_t body_len = 0;
        int expect_continue = 0;
        int v = http_body_validate(&req, 65536, &body_len, &expect_continue);
        if (v == 0 && body_len > 65536)
            __builtin_trap();
    }

    http_request_destroy(&req);
    return 0;
}
