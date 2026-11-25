#ifndef CONCURRENCY_H
#define CONCURRENCY_H
#include <stddef.h>
#include "proxy.h"
int concurrent_server_run(int listener, int root, size_t workers, size_t queue_limit,
                          size_t connection_limit, size_t body_limit, size_t body_timeout,
                          size_t idle_timeout, size_t max_requests, ProxyConfig *proxy_config);
#endif
