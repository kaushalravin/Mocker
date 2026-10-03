#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* mockerhost control-plane HTTP server: serves the deploy form and hands submissions to
 * clone_and_build.sh. Raw sockets, single-threaded accept() loop. User-submitted commands never
 * become host shell code (docs/mockerhost-design.md sections 5 and 7). */

#define LISTEN_PORT 8080
#define REQUEST_BUF_MAX 8192
#define FIELD_MAX 512
#define NAME_MAX_LEN 64

/* Paths are relative to mockerhost/, the directory the server must be launched from. */
#define STATIC_INDEX_PATH "static/index.html"
#define CLONE_AND_BUILD_SCRIPT "clone_and_build.sh"

typedef struct {
    char method[8];
    char path[256];
    char body[REQUEST_BUF_MAX];
} http_request_t;

static void url_decode(const char *in, char *out, size_t out_size)
{
    size_t oi = 0;
    for (size_t i = 0; in[i] != '\0' && oi + 1 < out_size; i++) {
        if (in[i] == '+') {
            out[oi++] = ' ';
        } else if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) &&
                   isxdigit((unsigned char)in[i + 2])) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            out[oi++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[oi++] = in[i];
        }
    }
    out[oi] = '\0';
}

/* Finds "key=value" in a urlencoded body and url_decode()s it into out; -1 if absent. */
static int extract_form_field(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while (p != NULL && *p != '\0') {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val_start = p + key_len + 1;
            const char *amp = strchr(val_start, '&');
            size_t val_len = amp != NULL ? (size_t)(amp - val_start) : strlen(val_start);
            if (val_len >= FIELD_MAX)
                val_len = FIELD_MAX - 1;

            char raw[FIELD_MAX];
            memcpy(raw, val_start, val_len);
            raw[val_len] = '\0';
            url_decode(raw, out, out_size);
            return 0;
        }
        p = strchr(p, '&');
        if (p != NULL)
            p++;
    }

    out[0] = '\0';
    return -1;
}

/* Reads one HTTP request: headers up to a blank line, then Content-Length bytes of body. */
static int read_request(int fd, http_request_t *req)
{
    char buf[REQUEST_BUF_MAX];
    size_t total = 0;
    char *header_end = NULL;

    while (total < sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0)
            return -1;
        total += (size_t)n;
        buf[total] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
        if (header_end != NULL)
            break;
    }
    if (header_end == NULL)
        return -1;

    if (sscanf(buf, "%7s %255s", req->method, req->path) != 2)
        return -1;

    long content_length = 0;
    const char *cl_header = strcasestr(buf, "Content-Length:");
    if (cl_header != NULL)
        content_length = strtol(cl_header + strlen("Content-Length:"), NULL, 10);
    if (content_length < 0)
        content_length = 0;
    if ((size_t)content_length >= sizeof(req->body))
        content_length = (long)sizeof(req->body) - 1;

    size_t header_len = (size_t)(header_end - buf) + 4;
    size_t body_so_far = total > header_len ? total - header_len : 0;
    if (body_so_far > (size_t)content_length)
        body_so_far = (size_t)content_length;
    memcpy(req->body, buf + header_len, body_so_far);

    while (body_so_far < (size_t)content_length) {
        ssize_t n = read(fd, req->body + body_so_far, (size_t)content_length - body_so_far);
        if (n <= 0)
            break;
        body_so_far += (size_t)n;
    }
    req->body[body_so_far] = '\0';

    return 0;
}

static void send_response(int fd, int status_code, const char *status_text,
                           const char *content_type, const char *body)
{
    char header[512];
    size_t body_len = strlen(body);
    int n = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status_code, status_text, content_type, body_len);
    if (n > 0)
        write(fd, header, (size_t)n);
    write(fd, body, body_len);
}

static void serve_index(int fd)
{
    FILE *f = fopen(STATIC_INDEX_PATH, "r");
    if (f == NULL) {
        send_response(fd, 500, "Internal Server Error", "text/plain",
                      "mockerhost: static/index.html not found - run mockerhost-server "
                      "from inside the mockerhost/ directory\n");
        return;
    }

    char body[16384];
    size_t len = fread(body, 1, sizeof(body) - 1, f);
    body[len] = '\0';
    fclose(f);

    send_response(fd, 200, "OK", "text/html", body);
}

/* Derives a valid container name from the repo name plus a time suffix, so redeploys don't collide. */
static void generate_app_name(const char *github_url, char *out, size_t out_size)
{
    const char *slash = strrchr(github_url, '/');
    const char *start = slash != NULL ? slash + 1 : github_url;

    char raw[128];
    size_t i = 0;
    for (; start[i] != '\0' && i < sizeof(raw) - 1; i++)
        raw[i] = start[i];
    raw[i] = '\0';

    size_t len = strlen(raw);
    if (len > 4 && strcmp(raw + len - 4, ".git") == 0)
        raw[len - 4] = '\0';

    char slug[56];
    size_t oi = 0;
    for (size_t ri = 0; raw[ri] != '\0' && oi + 1 < sizeof(slug); ri++) {
        unsigned char c = (unsigned char)raw[ri];
        slug[oi++] = (isalnum(c) || c == '_' || c == '-') ? (char)tolower(c) : '-';
    }
    slug[oi] = '\0';
    if (oi == 0)
        snprintf(slug, sizeof(slug), "app");

    long suffix = (long)(time(NULL) % 100000);
    snprintf(out, out_size, "%s-%05ld", slug, suffix);
}

static int valid_github_url(const char *url)
{
    const char *prefix = "https://github.com/";
    /* An exact prefix (not "contains github.com") blocks git transports like ext:: and file://,
     * and a leading '-' (docs/mockerhost-design.md section 4). */
    return strncmp(url, prefix, strlen(prefix)) == 0 && strlen(url) > strlen(prefix) &&
           strlen(url) < FIELD_MAX;
}

static int valid_port(const char *s)
{
    if (*s == '\0')
        return 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }
    long v = strtol(s, NULL, 10);
    return v >= 1 && v <= 65535;
}

/* Runs clone_and_build.sh with each field as its own argv element, never a shell string
 * (docs/mockerhost-design.md sections 2 and 5). Its stdout carries one "RESULT ..." line. */
static int run_clone_and_build(const char *name, const char *github_url, const char *build_cmd,
                                const char *run_cmd, const char *container_port,
                                char *result_line, size_t result_line_size)
{
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        snprintf(result_line, result_line_size, "RESULT error internal_pipe_failed");
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(result_line, result_line_size, "RESULT error internal_fork_failed");
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        /* Via /bin/sh so a lost executable bit (Windows-authored commit) doesn't matter. */
        char *argv[] = {"/bin/sh",           CLONE_AND_BUILD_SCRIPT, (char *)name,
                         (char *)github_url, (char *)build_cmd,      (char *)run_cmd,
                         (char *)container_port, NULL};
        execvp("/bin/sh", argv);
        perror("execvp clone_and_build.sh");
        _exit(127);
    }

    close(pipefd[1]);

    char buf[4096];
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(buf) - 1 &&
           (n = read(pipefd[0], buf + total, sizeof(buf) - 1 - total)) > 0)
        total += (size_t)n;
    buf[total] = '\0';
    close(pipefd[0]);

    waitpid(pid, NULL, 0);

    const char *found = strstr(buf, "RESULT ");
    if (found == NULL) {
        snprintf(result_line, result_line_size, "RESULT error no_result_line");
        return -1;
    }

    size_t i = 0;
    for (; found[i] != '\0' && found[i] != '\n' && found[i] != '\r' && i + 1 < result_line_size;
         i++)
        result_line[i] = found[i];
    result_line[i] = '\0';

    return 0;
}

static void handle_deploy(int fd, const char *body)
{
    char github_url[FIELD_MAX];
    char build_cmd[FIELD_MAX];
    char run_cmd[FIELD_MAX];
    char container_port[16];

    if (extract_form_field(body, "github_url", github_url, sizeof(github_url)) == -1 ||
        extract_form_field(body, "build_cmd", build_cmd, sizeof(build_cmd)) == -1 ||
        extract_form_field(body, "run_cmd", run_cmd, sizeof(run_cmd)) == -1 ||
        extract_form_field(body, "container_port", container_port, sizeof(container_port)) ==
            -1) {
        send_response(fd, 400, "Bad Request", "text/plain", "missing form field\n");
        return;
    }

    if (!valid_github_url(github_url)) {
        send_response(fd, 400, "Bad Request", "text/plain",
                      "github_url must start with https://github.com/\n");
        return;
    }
    if (!valid_port(container_port)) {
        send_response(fd, 400, "Bad Request", "text/plain",
                      "container_port must be a number between 1 and 65535\n");
        return;
    }
    if (build_cmd[0] == '\0' || run_cmd[0] == '\0') {
        send_response(fd, 400, "Bad Request", "text/plain", "build_cmd and run_cmd are required\n");
        return;
    }

    char name[NAME_MAX_LEN + 8];
    generate_app_name(github_url, name, sizeof(name));

    char result_line[256];
    run_clone_and_build(name, github_url, build_cmd, run_cmd, container_port, result_line,
                         sizeof(result_line));

    char status[16] = "";
    char detail[256] = "";
    sscanf(result_line, "RESULT %15s %255[^\n]", status, detail);

    char response_body[1024];
    if (strcmp(status, "ok") == 0) {
        /* detail is "<name> <hostport>"; only the name is needed. */
        char deployed_name[NAME_MAX_LEN + 8] = "";
        sscanf(detail, "%63s", deployed_name);
        /* This page is on :8080 but nginx serves the app on port 80, so build the link from
         * the browser's hostname without a port. */
        snprintf(response_body, sizeof(response_body),
                 "<html><body><h1>Deployed</h1><p>Live at "
                 "<a id=\"l\" href=\"#\">(enable JavaScript, or open "
                 "http://&lt;vm-ip&gt;/apps/%s/ on port 80)</a></p>"
                 "<script>var u=location.protocol+\"//\"+location.hostname+\"/apps/%s/\";"
                 "var a=document.getElementById(\"l\");a.href=u;a.textContent=u;</script>"
                 "</body></html>",
                 deployed_name, deployed_name);
        send_response(fd, 200, "OK", "text/html", response_body);
    } else {
        snprintf(response_body, sizeof(response_body),
                 "<html><body><h1>Deploy failed</h1><p>%s</p></body></html>", detail);
        send_response(fd, 502, "Bad Gateway", "text/html", response_body);
    }
}

int main(void)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(LISTEN_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 16) == -1) {
        perror("listen");
        return EXIT_FAILURE;
    }

    printf("mockerhost: listening on :%d (serving %s)\n", LISTEN_PORT, STATIC_INDEX_PATH);

    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd == -1) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        http_request_t req;
        if (read_request(client_fd, &req) == -1) {
            close(client_fd);
            continue;
        }

        if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/") == 0)
            serve_index(client_fd);
        else if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/deploy") == 0)
            handle_deploy(client_fd, req.body);
        else
            send_response(client_fd, 404, "Not Found", "text/plain", "not found\n");

        close(client_fd);
    }

    return EXIT_SUCCESS;
}
