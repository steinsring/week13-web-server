#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int parse_port(const char* text, unsigned* port)
{
    unsigned n = 0;
    if (!*text)
        return -1;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || n > 6553)
            return -1;
        n = n * 10 + (unsigned)(*text - '0');
        if (n > 65535)
            return -1;
    }
    if (!n)
        return -1;
    *port = n;
    return 0;
}

static int parse_authority(const char* text, ProxyOrigin* origin)
{
    if (!*text || strlen(text) >= sizeof(origin->authority))
        return -1;
    strcpy(origin->authority, text);
    const char *host = text, *end, *port = NULL;
    if (*host == '[') {
        end = strchr(++host, ']');
        if (!end || (end[1] && end[1] != ':'))
            return -1;
        if (end[1])
            port = end + 2;
    } else {
        end = strchr(host, ':');
        if (end)
            port = end + 1;
        else
            end = host + strlen(host);
    }
    size_t length = (size_t)(end - host);
    if (!length || length >= sizeof(origin->host))
        return -1;
    memcpy(origin->host, host, length);
    origin->host[length] = 0;
    if (text[0] == '[') {
        struct in6_addr address;
        if (inet_pton(AF_INET6, origin->host, &address) != 1)
            return -1;
    } else {
        for (const char* p = origin->host; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-'))
                return -1;
    }
    origin->port = origin->https ? 443 : 80;
    return port ? parse_port(port, &origin->port) : 0;
}

static int parse_origin(const char* text, ProxyOrigin* origin, int allow_https)
{
    memset(origin, 0, sizeof(*origin));
    if (strlen(text) >= sizeof(origin->text))
        return -1;
    const char* authority;
    if (!strncasecmp(text, "http://", 7))
        authority = text + 7;
    else if (allow_https && !strncasecmp(text, "https://", 8)) {
        authority = text + 8;
        origin->https = 1;
    } else
        return -1;
    if (strpbrk(authority, "/?#@\\"))
        return -1;
    if (parse_authority(authority, origin))
        return -1;
    strcpy(origin->text, text);
    return 0;
}

/* DNS는 시작 시 한 번만 수행한다. 항목 하나라도 잘못되면 전체 시작을 중단한다. */
int proxy_config_init(ProxyConfig* config, const char* public_url, const char* list,
    size_t connect_seconds, size_t idle_seconds)
{
    memset(config, 0, sizeof(*config));
    if (!connect_seconds || !idle_seconds || connect_seconds > INT64_MAX / 2000 || idle_seconds > INT64_MAX / 2000)
        return -1;
    if (parse_origin(public_url, &config->public_origin, 1) || !*list)
        return -1;
    size_t count = 1;
    for (const char* p = list; *p; ++p)
        if (*p == ',')
            ++count;
    if (count > SIZE_MAX / sizeof(*config->backends))
        return -1;
    config->backends = calloc(count, sizeof(*config->backends));
    if (!config->backends)
        return -1;
    const char* p = list;
    for (size_t i = 0; i < count; ++i) {
        const char* end = strchr(p, ',');
        size_t length = end ? (size_t)(end - p) : strlen(p);
        char value[512], service[6];
        if (!length || length >= sizeof(value))
            goto fail;
        memcpy(value, p, length);
        value[length] = 0;
        ProxyBackend* backend = &config->backends[i];
        if (parse_origin(value, &backend->origin, 0))
            goto fail;
        snprintf(service, sizeof(service), "%u", backend->origin.port);
        struct addrinfo hints = { 0 }, *addresses;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(backend->origin.host, service, &hints, &addresses))
            goto fail;
        struct addrinfo* selected = addresses;
        while (selected && (selected->ai_family != AF_INET && selected->ai_family != AF_INET6))
            selected = selected->ai_next;
        if (!selected || selected->ai_addrlen > sizeof(backend->address)) {
            freeaddrinfo(addresses);
            goto fail;
        }
        memcpy(&backend->address, selected->ai_addr, selected->ai_addrlen);
        backend->address_length = selected->ai_addrlen;
        freeaddrinfo(addresses);
        p = end ? end + 1 : p + length;
    }
    config->count = count;
    config->connect_ms = (int64_t)connect_seconds * 1000;
    config->idle_ms = (int64_t)idle_seconds * 1000;
    return 0;
fail:
    proxy_config_destroy(config);
    return -1;
}
void proxy_config_destroy(ProxyConfig* config)
{
    free(config->backends);
    memset(config, 0, sizeof(*config));
}
int proxy_route(const char* path)
{
    return !strcmp(path, "/api") || !strncmp(path, "/api/", 5);
}

/* DNS 별칭이나 부분 일치로 내부 주소를 추측하지 않는다. 등록된 origin만 변환한다. */
int proxy_rewrite_location(const ProxyConfig* config, const char* value, char* out, size_t capacity)
{
    for (const unsigned char* p = (const unsigned char*)value; *p; ++p) {
        if (*p <= 32 || *p >= 127 || *p == '\\')
            return -1;
        if (*p == '%') {
            if (!p[1] || !p[2] || !isxdigit(p[1]) || !isxdigit(p[2]))
                return -1;
            p += 2;
        }
    }
    if ((!strncasecmp(value, "http:", 5) && strncasecmp(value, "http://", 7))
        || (!strncasecmp(value, "https:", 6) && strncasecmp(value, "https://", 8)))
        return -1;
    if (!*value)
        return -1;
    const char* authority = NULL;
    if (!strncasecmp(value, "http://", 7))
        authority = value + 7;
    else if (!strncmp(value, "//", 2))
        authority = value + 2;
    else if (!strncasecmp(value, "https://", 8)) {
        ProxyOrigin external = { .https = 1 };
        const char* end = value + 8 + strcspn(value + 8, "/?#");
        char name[320];
        size_t n = (size_t)(end - (value + 8));
        if (n >= sizeof(name))
            return -1;
        memcpy(name, value + 8, n);
        name[n] = 0;
        if (parse_authority(name, &external))
            return -1;
    } else {
        const char* colon = strchr(value, ':');
        const char* boundary = value + strcspn(value, "/?#");
        if (colon && colon < boundary) {
            if (!isalpha((unsigned char)value[0]))
                return -1;
            for (const char* p = value + 1; p < colon; ++p)
                if (!isalnum((unsigned char)*p) && !strchr("+.-", *p))
                    return -1;
        }
    }
    const char* suffix = NULL;
    if (authority) {
        suffix = authority + strcspn(authority, "/?#");
        char name[320];
        size_t n = (size_t)(suffix - authority);
        if (n >= sizeof(name))
            return -1;
        memcpy(name, authority, n);
        name[n] = 0;
        ProxyOrigin location = { 0 };
        if (parse_authority(name, &location))
            return -1;
        int internal = 0;
        for (size_t i = 0; i < config->count; ++i)
            if (!strcasecmp(location.host, config->backends[i].origin.host) && location.port == config->backends[i].origin.port)
                internal = 1;
        if (internal) {
            int nout = snprintf(out, capacity, "%s%s", config->public_origin.text, suffix);
            return nout < 0 || (size_t)nout >= capacity ? -1 : 0;
        }
    }
    if (strlen(value) >= capacity)
        return -1;
    strcpy(out, value);
    return 0;
}
