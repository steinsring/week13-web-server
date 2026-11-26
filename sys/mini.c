#include <sys/stat.h>
#include <sys/socket.h>
#include <errno.h>
#include <stddef.h> //NULL여기있음
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <poll.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "server.h"
#include <netdb.h>
#include <sys/wait.h>
#include <stdint.h>
#include <limits.h>
#include <pthread.h>
#include "reverse_proxy.h"
#include <signal.h>

typedef struct sockaddr SA;

#define MAXLINE 8192
#define MAXBUF 8192



typedef struct ClientArgs
{
    int connfd;
    const char *document_root;
    struct ClientArgs *next;
} ClientArgs;

static void doit(int fd, const char *document_root);
int send_all(int fd, const char *buf, size_t len);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);
static int parse_request(int fd, char *buf, size_t size, const char *body, size_t body_received, const char *document_root);
static void handle_request(int fd, HttpRequest *request, size_t size, const char *document_root);
static void get_filetype(char *filename, char *filetype);
static void serve_static(int fd, char *filename, const char *document_root);
static int handle_post(int fd, HttpRequest *request, const char *document_root);
static void serve_dynamic(int fd, char *filename, char *cgiargs);
static void *handle_client(void *arg);

static char **backends;
static size_t backend_count, next_backend;
static const char *route_prefix = "/api/";
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t workers_done = PTHREAD_COND_INITIALIZER;
static ClientArgs *active_clients;
static volatile sig_atomic_t stopping;

static void stop_server(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

// listenfd 연결을 기다리는 소켓 connfd는 데이터를 주고 받는 소켓
int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    int first_backend = 3;
    if (argc > 3 && strncmp(argv[3], "--route=", 8) == 0) {
        route_prefix = argv[3] + 8;
        first_backend++;
    }
    if (argc - first_backend < 2 || (argc - first_backend) % 2 != 0 || route_prefix[0] != '/') {
        fprintf(stderr, "usage: %s <port> <document_root> [--route=/api/] <backend_host> <backend_port> [host port ...]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *document_root = argv[2];
    backends = argv + first_backend;
    backend_count = (size_t)(argc - first_backend) / 2;
    struct sigaction action = {0};
    action.sa_handler = stop_server;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction"); return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0)
    {
        fprintf(stderr, "서버 소켓을 열 수 없습니다.\n");
        return EXIT_FAILURE;
    }
    if (fcntl(listenfd, F_SETFD, FD_CLOEXEC) < 0)
    {
        close(listenfd);
        return EXIT_FAILURE;
    }
    while (!stopping)
    {
        struct pollfd listener = { .fd = listenfd, .events = POLLIN };
        int ready = poll(&listener, 1, 500);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (ready == 0 || stopping) continue;
        clientlen = sizeof(clientaddr);
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (connfd < 0)
        {
            continue;
        }
        if (fcntl(connfd, F_SETFD, FD_CLOEXEC) < 0)
        {
            close(connfd);
            continue;
        }
        if (getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0)
            printf("Accepted connection from (%s, %s)\n", hostname, port);
        ClientArgs *client = malloc(sizeof(*client));
        if (client == NULL)
        {
            close(connfd);
            continue;
        }
        client->connfd = connfd;
        client->document_root = document_root;
        struct timeval timeout = {5, 0};
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(connfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        pthread_mutex_lock(&state_lock);
        client->next = active_clients;
        active_clients = client;
        pthread_mutex_unlock(&state_lock);
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, client) != 0)
        {
            pthread_mutex_lock(&state_lock);
            ClientArgs **entry = &active_clients;
            while (*entry && *entry != client) entry = &(*entry)->next;
            if (*entry) *entry = client->next;
            pthread_mutex_unlock(&state_lock);
            close(connfd);
            free(client);
        }
    }
    close(listenfd);
    pthread_mutex_lock(&state_lock);
    for (ClientArgs *client = active_clients; client; client = client->next)
        shutdown(client->connfd, SHUT_RDWR);
    while (active_clients) pthread_cond_wait(&workers_done, &state_lock);
    pthread_mutex_unlock(&state_lock);
    return EXIT_SUCCESS;
}

// http요청 하나를 받아 응답을 보내는 전체과정을 담당한다.
static void doit(int fd, const char *document_root)
{
    size_t used = 0;
    ssize_t temp = 0;

    char buf[MAXLINE];
    char *header_end = NULL;
    while (used < sizeof(buf) - 1)
    {
        temp = recv(fd, buf + used, sizeof(buf) - 1 - used, 0);
        if (temp < 0 && errno == EINTR)
        {
            continue;
        }
        if (temp <= 0)
        {
            return;
        }
        used += (size_t)temp;
        buf[used] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (header_end != NULL)
        {
            break;
        }
    }
    if (header_end == NULL)
    {
        clienterror(fd, "", "431", "Request Header Fields Too Large", "Request headers too large");
        return;
    }
    size_t header_bytes = (size_t)(header_end - buf) + 4;
    size_t body_received = used - header_bytes;
    if (parse_request(fd, buf, header_bytes, buf + header_bytes, body_received, document_root) < 0)
        clienterror(fd, "", "400", "Bad Request", "Invalid HTTP request");
}

int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t temp_sent = 0;
        temp_sent = send(fd, buf + sent, len - sent, 0);
        if (temp_sent > 0)
        {
            sent += temp_sent;
        }
        else if (temp_sent < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return -1;
        }
    }
    return 0;
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];
    const char *video = "";
    if (strcmp(errnum, "404") == 0)
    {
        video =
            "<div style=\"max-width: 1600px; margin: 20px auto; "
            "background: #f43f5e; text-align: center;\">"
            "<h2 style=\"color: white; margin: 0; padding: 10px;\">MV</h2>"
            "<iframe style=\"width: 100%; aspect-ratio: 16 / 9; border: 0;\" "
            "src=\"https://www.youtube.com/embed/zhHB4dZTChw\" "
            "title=\"KiiiKiii 404 (New Era) MV\" "
            "allow=\"autoplay; encrypted-media; picture-in-picture\" "
            "allowfullscreen></iframe>"
            "</div>";
    }
    snprintf(body, sizeof(body),
             "<html><title>Tiny Error</title>"
             "<body><h1>%s %s</h1>"
             "<p>%s: %s</p>"
             "%s"
             "<hr><em>The Mini Web server</em></body></html>",
             errnum, shortmsg, longmsg, "", video);
    (void)cause;
    snprintf(buf, sizeof(buf), "HTTP/1.1 %s %s\r\nConnection: close\r\n", errnum, shortmsg);
    send_all(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Content-type: text/html\r\n");
    send_all(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "Content-length: %d\r\n\r\n", (int)strlen(body));
    send_all(fd, buf, strlen(buf));
    send_all(fd, body, strlen(body));
}

static int parse_request(int fd, char *buf, size_t size, const char *body, size_t body_received, const char *document_root)
{
    HttpRequest request = {0};
    request.raw_headers = buf;
    request.headers_length = size;
    request.body = body;
    request.body_received = body_received;
    const char *line_end = strstr(buf, "\r\n");
    if (line_end == NULL)
    {
        return -1;
    }
    if ((size_t)(line_end - buf) + 2 > size)
    {
        return -1;
    }
    const char *header = line_end + 2;
    int has_length = 0;
    while (1)
    {
        const char *header_final = strstr(header, "\r\n");
        if (header_final == NULL)
        {
            return -1;
        }
        if (header == header_final)
        {
            break;
        }
        const char *colon = memchr(header, ':', (size_t)(header_final - header));
        if (!colon || colon == header) return -1;
        for (const char *ch = header; ch < colon; ch++)
            if (!isalnum((unsigned char)*ch) && !strchr("!#$%&'*+-.^_`|~", *ch)) return -1;
        for (const char *ch = colon + 1; ch < header_final; ch++)
            if (((unsigned char)*ch < 32 && *ch != '\t') || *ch == 127) return -1;
        if (strncasecmp(header, "Connection:", 11) == 0)
        {
            const char *value = colon + 1;
            while (value < header_final && (*value == ' ' || *value == '\t')) value++;
            const char *end = header_final;
            while (end > value && (end[-1] == ' ' || end[-1] == '\t')) end--;
            size_t length = (size_t)(end - value);
            if (!((length == 5 && !strncasecmp(value, "close", length)) ||
                  (length == 10 && !strncasecmp(value, "keep-alive", length))))
                return -1;
        }
        if (strncasecmp(header, "Transfer-Encoding:", 18) == 0 ||
            strncasecmp(header, "Expect:", 7) == 0)
            return -1;
        if (strncasecmp(header, "Host:", 5) == 0)
        {
            const char *value = header + 5;
            while (value < header_final && (*value == ' ' || *value == '\t'))
            {
                value++;
            }
            size_t host_len = (size_t)(header_final - value);
            if (!host_len || request.host[0] || host_len >= sizeof(request.host))
            {
                return -1;
            }
            memcpy(request.host, value, host_len);
            request.host[host_len] = '\0';
        }
        else if (strncasecmp(header, "Content-Length:", 15) == 0)
        {
            if (has_length++) return -1;
            const char *value = header + 15;
            while (value < header_final && (*value == ' ' || *value == '\t'))
            {
                value++;
            }
            char *number_end;
            errno = 0;
            if (value == header_final || *value < '0' || *value > '9')
            {
                return -1;
            }
            unsigned long long length = strtoull(value, &number_end, 10);
            while (number_end < header_final && (*number_end == ' ' || *number_end == '\t')) number_end++;
            if (errno == ERANGE || number_end != header_final || length > 1024 * 1024)
            {
                return -1;
            }
            request.content_length = (size_t)length;
        }
        header = header_final + 2;
    }
    printf("Host: %s\n", request.host);
    printf("Content-Length: %zu\n", request.content_length);
    char extra;
    char *request_line_end = buf + (line_end - buf);
    *request_line_end = '\0';
    int count = sscanf(buf, "%15s %2047s %15s %c", request.method, request.uri, request.version, &extra);
    *request_line_end = '\r';
    if (count != 3 || request.uri[0] != '/' ||
        (strcmp(request.version, "HTTP/1.0") && strcmp(request.version, "HTTP/1.1")) ||
        (!strcmp(request.version, "HTTP/1.1") && !request.host[0]) ||
        (!strcmp(request.method, "POST") && !has_length))
    {
        return -1;
    }
    if (strcmp(request.method, "GET") == 0)
    {
        printf("GET 요청: %s\n", request.uri);
        handle_request(fd, &request, size, document_root);
    }
    else if (strcmp(request.method, "POST") == 0)
    {
        printf("POST 요청: %s\n", request.uri);
        handle_request(fd, &request, size, document_root);
    }
    else
    {
        return -1;
    }

    return 0;
}

static void handle_request(int fd, HttpRequest *request, size_t size, const char *document_root)
{
    (void)size;
    if (strncmp(request->uri, route_prefix, strlen(route_prefix)) == 0)
    {
        pthread_mutex_lock(&state_lock);
        size_t selected = next_backend++ % backend_count;
        pthread_mutex_unlock(&state_lock);
        reverse_proxy(fd, request, backends[selected * 2], backends[selected * 2 + 1]);
        return;
    }
    if (strcmp(request->method, "GET") == 0)
    {
        if (strncmp(request->uri, "/cgi-bin/", 9) == 0)
        {
            const char *query = strchr(request->uri, '?');
            size_t path_len = query ? (size_t)(query - request->uri) : strlen(request->uri);
            if (path_len != strlen("/cgi-bin/test") || strncmp(request->uri, "/cgi-bin/test", path_len))
                clienterror(fd, "", "404", "Not Found", "CGI script not found");
            else
                serve_dynamic(fd, "./cgi-bin/test", query ? (char *)query + 1 : "");
            return;
        }
        serve_static(fd, request->uri, document_root);
    }
    else
    {
        handle_post(fd, request, document_root);
    }
}

static void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html"))
    {
        strcpy(filetype, "text/html");
    }
    else if (strstr(filename, ".gif"))
    {
        strcpy(filetype, "image/gif");
    }
    else if (strstr(filename, ".png"))
    {
        strcpy(filetype, "image/png");
    }
    else if (strstr(filename, ".jpg"))
    {
        strcpy(filetype, "image/jpeg");
    }
    else if (strstr(filename, ".css"))
    {
        strcpy(filetype, "text/css");
    }
    else if (strstr(filename, ".js"))
    {
        strcpy(filetype, "text/javascript");
    }
    else
    {
        strcpy(filetype, "text/plain");
    }
}

/* Resolve the parent within the document root before opening the file. */
static int file_path(int fd, const char *root, const char *uri, char *path, size_t capacity)
{
    size_t length = strcspn(uri, "?");
    if (length < 2 || length >= capacity || uri[0] != '/')
    {
        clienterror(fd, "", "400", "Bad Request", "Invalid file path");
        return -1;
    }
    char relative[MAXLINE];
    size_t written = 0;
    for (size_t i = 1; i < length; i++)
    {
        unsigned char ch = (unsigned char)uri[i];
        if (ch == '%')
        {
            if (i + 2 >= length || !isxdigit((unsigned char)uri[i + 1]) ||
                !isxdigit((unsigned char)uri[i + 2]))
            {
                clienterror(fd, "", "400", "Bad Request", "Invalid path encoding");
                return -1;
            }
            char hex[] = {uri[i + 1], uri[i + 2], '\0'};
            ch = (unsigned char)strtoul(hex, NULL, 16);
            i += 2;
        }
        if (ch < 32 || ch == '\\')
        {
            clienterror(fd, "", "400", "Bad Request", "Invalid file path");
            return -1;
        }
        relative[written++] = (char)ch;
    }
    relative[written] = '\0';

    char *name = strrchr(relative, '/');
    const char *parent = ".";
    if (name)
    {
        *name++ = '\0';
        parent = relative;
    }
    else
    {
        name = relative;
    }
    if (!*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    {
        clienterror(fd, "", "403", "Forbidden", "Invalid file path");
        return -1;
    }

    char root_path[PATH_MAX], parent_path[PATH_MAX];
    if (!realpath(root, root_path))
    {
        clienterror(fd, "", "500", "Internal Server Error", "Cannot open document root");
        return -1;
    }
    int n = snprintf(path, capacity, "%s/%s", root_path, parent);
    if (n < 0 || (size_t)n >= capacity)
    {
        clienterror(fd, "", "414", "URI Too Long", "Requested path is too long");
        return -1;
    }
    if (!realpath(path, parent_path))
    {
        if (errno == EACCES || errno == ELOOP)
            clienterror(fd, "", "403", "Forbidden", "Permission denied");
        else if (errno == ENOENT || errno == ENOTDIR)
            clienterror(fd, "", "404", "Not Found", "Directory not found");
        else
            clienterror(fd, "", "500", "Internal Server Error", "Could not resolve directory");
        return -1;
    }
    size_t root_len = strlen(root_path);
    if (strcmp(root_path, "/") != 0 &&
        (strncmp(parent_path, root_path, root_len) != 0 ||
         (parent_path[root_len] != '\0' && parent_path[root_len] != '/')))
    {
        clienterror(fd, "", "403", "Forbidden", "Path outside document root");
        return -1;
    }
    n = snprintf(path, capacity, "%s/%s", parent_path, name);
    if (n < 0 || (size_t)n >= capacity)
    {
        clienterror(fd, "", "414", "URI Too Long", "Requested path is too long");
        return -1;
    }
    return 0;
}

static void serve_static(int fd, char *filename, const char *document_root)
{
    int filesize = 0;
    char *srcp, filetype[MAXLINE], path[MAXLINE];
    if (filename[0] == '/' && strcspn(filename, "?") == 1)
    {
        clienterror(fd, filename, "404", "Not Found", "File not found");
        return;
    }
    if (file_path(fd, document_root, filename, path, sizeof(path)) < 0)
    {
        return;
    }
    int srcfd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int error = errno;
    if (srcfd < 0)
    {
        if (error == EACCES || error == ELOOP)
            clienterror(fd, filename, "403", "Forbidden", "Permission denied");
        else if (error == ENOENT || error == ENOTDIR)
            clienterror(fd, filename, "404", "Not Found", "File not found");
        else
            clienterror(fd, filename, "500", "Internal Server Error", "Could not open file");
        return;
    }
    struct stat file_info;
    if (fstat(srcfd, &file_info) < 0)
    {
        close(srcfd);
        clienterror(fd, filename, "500", "Internal Server Error",
                    "Could not read file information");
        return;
    }
    if (!S_ISREG(file_info.st_mode) || file_info.st_size < 0 || file_info.st_size > INT_MAX)
    {
        close(srcfd);
        clienterror(fd, filename, "500", "Internal Server Error",
                    "Cannot serve this file");
        return;
    }
    filesize = (int)file_info.st_size;
    srcp = NULL;
    if (filesize > 0)
    {
        srcp = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
        if (srcp == MAP_FAILED)
        {
            close(srcfd);
            clienterror(fd, filename, "500", "Internal Server Error",
                        "Could not map file");
            return;
        }
    }
    close(srcfd);
    get_filetype(strrchr(path, '/') + 1, filetype);
    // sprintf(buf,"HTTP/1.1 200 OK\r\n");
    char buf[MAXBUF] = "HTTP/1.1 200 OK\r\n";
    size_t len = strlen(buf);
    snprintf(buf + len, sizeof(buf) - len, "Server:Mini Web Server\r\n");
    len = strlen(buf);
    snprintf(buf + len, sizeof(buf) - len, "Connection: close\r\n");
    len = strlen(buf);
    snprintf(buf + len, sizeof(buf) - len, "Content-length: %d\r\n", filesize);
    len = strlen(buf);
    snprintf(buf + len, sizeof(buf) - len, "Content-type: %s\r\n\r\n", filetype);
    if (send_all(fd, buf, strlen(buf)) < 0)
    {
        if (filesize > 0)
        {
            munmap(srcp, filesize);
        }
        return;
    }

    printf("Response headers:\n");
    printf("%s", buf);

    if (filesize > 0)
    {
        send_all(fd, srcp, filesize);
        munmap(srcp, filesize);
    }
}

static int handle_post(int fd, HttpRequest *request, const char *document_root)
{
    char buf[MAXLINE], path[MAXLINE];
    if (file_path(fd, document_root, request->uri, path, sizeof(path)) < 0) return -1;
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (file < 0)
    {
        int error = errno;
        if (error == EEXIST)
            clienterror(fd, "", "409", "Conflict", "File already exists");
        else if (error == EACCES || error == EROFS)
            clienterror(fd, "", "403", "Forbidden", "Permission denied");
        else
            clienterror(fd, "", "500", "Internal Server Error", "Could not create file");
        return -1;
    }
    FILE *fp = fdopen(file, "wb");
    if (fp == NULL)
    {
        close(file);
        unlink(path);
        clienterror(fd, "", "500", "Internal Server Error", "Could not open output stream");
        return -1;
    }
    size_t total = request->body_received;
    if (total > request->content_length)
    {
        total = request->content_length;
    }
    if (fwrite(request->body, 1, total, fp) != total)
    {
        goto write_error;
    }
    while (total < request->content_length)
    {
        size_t remaining = request->content_length - total;
        if (remaining > sizeof(buf))
        {
            remaining = sizeof(buf);
        }
        ssize_t n = recv(fd, buf, remaining, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) goto write_error;
        total += n;
    }
    int result = fclose(fp);
    if (total != request->content_length || result != 0)
    {
        unlink(path);
        if (result != 0)
            clienterror(fd, "", "500", "Internal Server Error", "Could not save file");
        else
            clienterror(fd, "", "400", "Bad Request", "Incomplete request body");
        return -1;
    }
    const char response[] = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    return send_all(fd, response, sizeof(response) - 1);

write_error:
    fclose(fp);
    unlink(path);
    clienterror(fd, "", "500", "Internal Server Error", "Could not save file");
    return -1;
}

static void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    if (access(filename, X_OK) < 0)
    {
        if (errno == ENOENT)
            clienterror(fd, "", "404", "Not Found", "CGI script not found");
        else
            clienterror(fd, "", "500", "Internal Server Error", "CGI script cannot run");
        return;
    }

    char query[MAXLINE];
    int length = snprintf(query, sizeof(query), "QUERY_STRING=%s", cgiargs);
    if (length < 0 || (size_t)length >= sizeof(query))
    {
        clienterror(fd, "", "500", "Internal Server Error", "CGI setup failed");
        return;
    }
    char *args[] = {filename, NULL};
    char *env[] = {query, NULL};
    pid_t child = fork();
    if (child < 0)
    {
        clienterror(fd, "", "500", "Internal Server Error", "CGI launch failed");
        return;
    }
    if (child == 0)
    {
        const char status[] = "HTTP/1.1 200 OK\r\nConnection: close\r\n";
        if (dup2(fd, STDOUT_FILENO) < 0)
            _exit(127);
        if (fd != STDOUT_FILENO)
            close(fd);
        if (send_all(STDOUT_FILENO, status, sizeof(status) - 1) < 0)
            _exit(127);
        execve(filename, args, env);
        _exit(127);
    }
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
}

static void *handle_client(void *arg)
{
    pthread_detach(pthread_self());
    ClientArgs *client = arg;
    doit(client->connfd, client->document_root);
    pthread_mutex_lock(&state_lock);
    ClientArgs **entry = &active_clients;
    while (*entry && *entry != client) entry = &(*entry)->next;
    if (*entry) *entry = client->next;
    close(client->connfd);
    pthread_cond_broadcast(&workers_done);
    pthread_mutex_unlock(&state_lock);
    free(client);
    return NULL;
}