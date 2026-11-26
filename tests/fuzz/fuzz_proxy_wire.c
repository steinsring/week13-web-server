#include "../../proxy.c"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * libFuzzer 하네스 #3: 리버스 프록시 백엔드 응답 파서(chunked/헤더/트레일러) + Location 재작성
 * - 백엔드가 깨진 HTTP 응답이나 악성 chunked 프레이밍을 보냈을 때 502로 안전 차단되는지,
 *   메모리 접근 오류·무한 루프·누수가 없는지 검증한다.
 */
static ProxyConfig g_cfg;
static int g_cfg_ready = 0;

static void init_cfg(void)
{
    if (g_cfg_ready)
        return;
    if (proxy_config_init(&g_cfg, "http://localhost:8081",
                          "http://127.0.0.1:8001,http://127.0.0.1:8002,http://[::1]:8003",
                          5, 3) != 0)
        __builtin_trap();
    g_cfg_ready = 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    init_cfg();
    if (size < 2)
        return 0;

    /* 1) Location 재작성 퍼징 */
    size_t loc_len = size > 511 ? 511 : size;
    char loc_in[512];
    char loc_out[1024];
    memcpy(loc_in, data, loc_len);
    loc_in[loc_len] = '\0';
    (void)proxy_rewrite_location(&g_cfg, loc_in, loc_out, sizeof(loc_out));

    /* 2) 백엔드 HTTP 응답 스트림 파서 퍼징 (2조각 분할 공급) */
    const uint8_t *wire = data + 1;
    size_t wire_len = size - 1;
    if (wire_len > PROXY_BUFFER)
        wire_len = PROXY_BUFFER;
    size_t split = wire_len ? ((size_t)data[0] % (wire_len + 1)) : 0;

    Proxy *p = calloc(1, sizeof(*p));
    if (!p)
        return 0;
    p->fd = -1;
    p->config = &g_cfg;
    p->phase = RESPONSE_READING;
    p->deadline = 10000;

    size_t parts[3] = {0, split, wire_len};
    int status = 0;
    for (unsigned i = 0; i < 2 && !status && p->phase != FINISHED; ++i) {
        size_t n = parts[i + 1] - parts[i];
        if (n == 0)
            continue;
        memcpy(p->input, wire + parts[i], n);
        p->input_length = n;
        p->input_position = 0;

        unsigned guard = 0;
        do {
            status = pump_input(p);
            if (status)
                break;
            if (p->output_length)
                proxy_consume_output(p, p->output_length, 0);
            if (++guard > 16384)
                __builtin_trap();
        } while (p->input_position < p->input_length && p->phase != FINISHED);
    }

    proxy_destroy(p);
    return 0;
}
