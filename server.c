/*
 * server.c  |  Mbake Kekia Portfolio Web Server
 *
 * A small HTTP/1.1 web server written in C with POSIX sockets.
 *
 *   Static files:   GET /, /style.css, /script.js ...   (served from ./public)
 *   Visit counter:  GET  /api/visits   -> {"visits": N}
 *                   POST /api/visits   -> increments, then {"visits": N}
 *   Contact form:   POST /api/contact  -> saves to data/messages.txt
 *
 * Security features:
 *   - Path traversal protection (realpath check against the web root)
 *   - Request size limits and socket timeouts
 *   - Input validation and sanitizing on the contact form
 *   - Security headers (CSP, nosniff, frame denial)
 *   - File locking so concurrent requests do not corrupt data
 *
 * Build:  make            (or: gcc -Wall -Wextra -O2 -o server server.c)
 * Run:    ./server        (default port 8080)
 *         ./server 3000   (custom port)
 *
 * Works on Linux and macOS. On Windows, use WSL.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT     8080
#define WEB_ROOT         "public"
#define DATA_DIR         "data"
#define VISITS_FILE      DATA_DIR "/visits.txt"
#define MESSAGES_FILE    DATA_DIR "/messages.txt"

#define MAX_HEADER_SIZE  8192
#define MAX_BODY_SIZE    8192
#define SOCKET_TIMEOUT_S 5
#define BACKLOG          32

#define MAX_NAME_LEN     100
#define MAX_EMAIL_LEN    150
#define MAX_MESSAGE_LEN  2000

static char web_root_real[PATH_MAX];
static volatile sig_atomic_t running = 1;

/* ------------------------------------------------------------------ */
/*  Request structure                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    char method[8];
    char path[1024];
    char *body;
    size_t body_len;
} http_request;

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */
static void on_sigint(int sig) {
    (void)sig;
    running = 0;
}

static void log_request(const char *ip, const char *method, const char *path, int status) {
    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] %-15s %-6s %-30s %d\n", ts, ip, method, path, status);
    fflush(stdout);
}

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static const char *status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "Unknown";
    }
}

static const char *mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".html")) return "text/html; charset=utf-8";
    if (!strcasecmp(ext, ".css"))  return "text/css; charset=utf-8";
    if (!strcasecmp(ext, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcasecmp(ext, ".json")) return "application/json";
    if (!strcasecmp(ext, ".png"))  return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".gif"))  return "image/gif";
    if (!strcasecmp(ext, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(ext, ".ico"))  return "image/x-icon";
    if (!strcasecmp(ext, ".webp")) return "image/webp";
    if (!strcasecmp(ext, ".pdf"))  return "application/pdf";
    if (!strcasecmp(ext, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcasecmp(ext, ".woff2"))return "font/woff2";
    return "application/octet-stream";
}

/* Writes the status line plus the headers every response gets. */
static int send_headers(int fd, int code, const char *type, size_t len) {
    char hdr[1024];
    int n = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\n"
        "Server: mbake-c-server/1.0\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: strict-origin-when-cross-origin\r\n"
        "Content-Security-Policy: default-src 'self'; "
            "style-src 'self' https://fonts.googleapis.com; "
            "font-src https://fonts.gstatic.com; "
            "img-src 'self' data:; connect-src 'self'\r\n"
        "\r\n",
        code, status_text(code), type, len);
    return send_all(fd, hdr, (size_t)n);
}

static void send_json(int fd, int code, const char *json) {
    send_headers(fd, code, "application/json", strlen(json));
    send_all(fd, json, strlen(json));
}

static void send_error_page(int fd, int code) {
    char body[256];
    snprintf(body, sizeof body,
        "<!DOCTYPE html><html><body style=\"font-family:sans-serif;padding:40px\">"
        "<h1>%d %s</h1><p><a href=\"/\">Back to home</a></p></body></html>",
        code, status_text(code));
    send_headers(fd, code, "text/html; charset=utf-8", strlen(body));
    send_all(fd, body, strlen(body));
}

/* Decodes %XX sequences and '+' (as space) in place. Returns -1 on bad input. */
static int url_decode(char *s) {
    char *out = s;
    for (; *s; s++, out++) {
        if (*s == '+') {
            *out = ' ';
        } else if (*s == '%') {
            if (!isxdigit((unsigned char)s[1]) || !isxdigit((unsigned char)s[2])) return -1;
            char hex[3] = { s[1], s[2], 0 };
            *out = (char)strtol(hex, NULL, 16);
            if (*out == '\0') return -1;          /* reject null bytes */
            s += 2;
        } else {
            *out = *s;
        }
    }
    *out = '\0';
    return 0;
}

/* Copies the value for `key` from an x-www-form-urlencoded body. */
static int form_value(const char *body, const char *key, char *out, size_t out_size) {
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= out_size) return -1;
            memcpy(out, v, len);
            out[len] = '\0';
            return url_decode(out);
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return -1;
}

/* Removes control characters so input cannot forge extra lines in the log.
 * When keep_breaks is set, line breaks become spaces instead of vanishing. */
static void sanitize(char *s, int keep_breaks) {
    char *out = s;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\n' && keep_breaks) { *out++ = ' '; continue; }
        if (c < 0x20 || c == 0x7f) continue;
        *out++ = (char)c;
    }
    *out = '\0';
}

static int valid_email(const char *e) {
    const char *at = strchr(e, '@');
    if (!at || at == e || strchr(at + 1, '@')) return 0;
    const char *dot = strrchr(at, '.');
    if (!dot || dot == at + 1 || dot[1] == '\0') return 0;
    for (; *e; e++) if (isspace((unsigned char)*e)) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Request reading and parsing                                        */
/* ------------------------------------------------------------------ */
static int read_request(int fd, http_request *req, char *buf, size_t buf_size) {
    size_t total = 0;
    char *header_end = NULL;

    /* Read until we have the full header block. */
    while (!header_end) {
        if (total >= MAX_HEADER_SIZE) return 413;
        ssize_t n = recv(fd, buf + total, MAX_HEADER_SIZE - total, 0);
        if (n <= 0) return -1;
        total += (size_t)n;
        buf[total] = '\0';
        header_end = strstr(buf, "\r\n\r\n");
    }

    /* Request line: METHOD PATH VERSION */
    char version[16];
    if (sscanf(buf, "%7s %1023s %15s", req->method, req->path, version) != 3) return 400;
    if (strncmp(version, "HTTP/1.", 7) != 0) return 400;

    /* Content-Length (case-insensitive search). */
    size_t content_len = 0;
    for (char *line = strstr(buf, "\r\n"); line && line < header_end; line = strstr(line + 2, "\r\n")) {
        if (strncasecmp(line + 2, "Content-Length:", 15) == 0) {
            long v = strtol(line + 17, NULL, 10);
            if (v < 0) return 400;
            content_len = (size_t)v;
        }
    }
    if (content_len > MAX_BODY_SIZE) return 413;

    /* Body: whatever came with the headers, then the rest. */
    char *body_start = header_end + 4;
    size_t have = total - (size_t)(body_start - buf);
    if (body_start + content_len >= buf + buf_size) return 413;

    while (have < content_len) {
        ssize_t n = recv(fd, body_start + have, content_len - have, 0);
        if (n <= 0) return -1;
        have += (size_t)n;
    }
    body_start[content_len] = '\0';
    req->body = body_start;
    req->body_len = content_len;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Data storage (with file locks)                                     */
/* ------------------------------------------------------------------ */
static long update_visits(int increment) {
    int fd = open(VISITS_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    flock(fd, LOCK_EX);

    char buf[32] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    long count = (n > 0) ? strtol(buf, NULL, 10) : 0;

    if (increment) {
        count++;
        int len = snprintf(buf, sizeof buf, "%ld\n", count);
        lseek(fd, 0, SEEK_SET);
        if (ftruncate(fd, 0) == 0 && write(fd, buf, (size_t)len) != len) count = -1;
    }

    flock(fd, LOCK_UN);
    close(fd);
    return count;
}

static int save_message(const char *ip, const char *name, const char *email, const char *msg) {
    int fd = open(MESSAGES_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return -1;
    flock(fd, LOCK_EX);

    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));

    char entry[MAX_NAME_LEN * 3 + MAX_EMAIL_LEN + MAX_MESSAGE_LEN * 3 + 512];
    int len = snprintf(entry, sizeof entry,
        "----------------------------------------\n"
        "Date:    %s\nFrom:    %s <%s>\nIP:      %s\nMessage: %s\n\n",
        ts, name, email, ip, msg);
    if (len < 0 || (size_t)len >= sizeof entry) len = (int)strlen(entry);

    int rc = (len > 0 && write(fd, entry, (size_t)len) == len) ? 0 : -1;
    flock(fd, LOCK_UN);
    close(fd);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Route handlers                                                     */
/* ------------------------------------------------------------------ */
static int handle_visits(int fd, const http_request *req) {
    int is_post = strcmp(req->method, "POST") == 0;
    if (!is_post && strcmp(req->method, "GET") != 0) {
        send_json(fd, 405, "{\"ok\":false,\"error\":\"method not allowed\"}");
        return 405;
    }
    long count = update_visits(is_post);
    if (count < 0) {
        send_json(fd, 500, "{\"ok\":false,\"error\":\"storage error\"}");
        return 500;
    }
    char json[64];
    snprintf(json, sizeof json, "{\"ok\":true,\"visits\":%ld}", count);
    send_json(fd, 200, json);
    return 200;
}

static int handle_contact(int fd, const http_request *req, const char *ip) {
    if (strcmp(req->method, "POST") != 0) {
        send_json(fd, 405, "{\"ok\":false,\"error\":\"method not allowed\"}");
        return 405;
    }

    char name[MAX_NAME_LEN * 3 + 1], email[MAX_EMAIL_LEN * 3 + 1], msg[MAX_MESSAGE_LEN * 3 + 1];

    if (form_value(req->body, "name", name, sizeof name) != 0 ||
        form_value(req->body, "email", email, sizeof email) != 0 ||
        form_value(req->body, "message", msg, sizeof msg) != 0) {
        send_json(fd, 400, "{\"ok\":false,\"error\":\"missing or invalid fields\"}");
        return 400;
    }

    sanitize(name, 0);
    sanitize(email, 0);
    sanitize(msg, 1);

    /* Byte limits allow for multi-byte UTF-8 characters (up to 3 bytes each). */
    if (strlen(name) == 0 || strlen(name) > MAX_NAME_LEN * 3 ||
        !valid_email(email) || strlen(email) > MAX_EMAIL_LEN ||
        strlen(msg) < 10 || strlen(msg) > MAX_MESSAGE_LEN * 3) {
        send_json(fd, 400, "{\"ok\":false,\"error\":\"validation failed\"}");
        return 400;
    }

    if (save_message(ip, name, email, msg) != 0) {
        send_json(fd, 500, "{\"ok\":false,\"error\":\"storage error\"}");
        return 500;
    }

    printf("    new message from %s <%s>\n", name, email);
    send_json(fd, 200, "{\"ok\":true}");
    return 200;
}

static int handle_static(int fd, const http_request *req) {
    int head_only = strcmp(req->method, "HEAD") == 0;
    if (!head_only && strcmp(req->method, "GET") != 0) {
        send_error_page(fd, 405);
        return 405;
    }

    /* Strip query string and fragment, then decode. */
    char path[1024];
    strncpy(path, req->path, sizeof path - 1);
    path[sizeof path - 1] = '\0';
    char *q = strpbrk(path, "?#");
    if (q) *q = '\0';
    if (url_decode(path) != 0 || path[0] != '/') {
        send_error_page(fd, 400);
        return 400;
    }

    /* Quick reject of traversal attempts before touching the filesystem. */
    if (strstr(path, "..")) {
        send_error_page(fd, 403);
        return 403;
    }

    char full[PATH_MAX + sizeof path + 16];
    int written;
    if (strcmp(path, "/") == 0)
        written = snprintf(full, sizeof full, "%s/index.html", web_root_real);
    else
        written = snprintf(full, sizeof full, "%s%s", web_root_real, path);
    if (written < 0 || (size_t)written >= PATH_MAX) {
        send_error_page(fd, 404);
        return 404;
    }

    /* Resolve symlinks and confirm the file is still inside the web root. */
    char resolved[PATH_MAX];
    if (!realpath(full, resolved)) {
        send_error_page(fd, 404);
        return 404;
    }
    size_t root_len = strlen(web_root_real);
    if (strncmp(resolved, web_root_real, root_len) != 0 ||
        (resolved[root_len] != '/' && resolved[root_len] != '\0')) {
        send_error_page(fd, 403);
        return 403;
    }

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_error_page(fd, 404);
        return 404;
    }

    int file = open(resolved, O_RDONLY);
    if (file < 0) {
        send_error_page(fd, 500);
        return 500;
    }

    send_headers(fd, 200, mime_type(resolved), (size_t)st.st_size);
    if (!head_only) {
        char chunk[16384];
        ssize_t n;
        while ((n = read(file, chunk, sizeof chunk)) > 0)
            if (send_all(fd, chunk, (size_t)n) != 0) break;
    }
    close(file);
    return 200;
}

/* ------------------------------------------------------------------ */
/*  Connection handler                                                 */
/* ------------------------------------------------------------------ */
static void handle_client(int fd, const char *ip) {
    struct timeval tv = { SOCKET_TIMEOUT_S, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    static char buf[MAX_HEADER_SIZE + MAX_BODY_SIZE + 2];
    http_request req;
    memset(&req, 0, sizeof req);

    int rc = read_request(fd, &req, buf, sizeof buf);
    if (rc < 0) return;                         /* client went away */
    if (rc > 0) {
        send_error_page(fd, rc);
        log_request(ip, req.method[0] ? req.method : "?", req.path[0] ? req.path : "?", rc);
        return;
    }

    int status;
    if (strncmp(req.path, "/api/visits", 11) == 0)
        status = handle_visits(fd, &req);
    else if (strncmp(req.path, "/api/contact", 12) == 0)
        status = handle_contact(fd, &req, ip);
    else
        status = handle_static(fd, &req);

    log_request(ip, req.method, req.path, status);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Usage: %s [port]\n", argv[0]);
            return 1;
        }
    }

    if (!realpath(WEB_ROOT, web_root_real)) {
        fprintf(stderr, "Error: cannot find the '%s' folder. Run the server from the project folder.\n", WEB_ROOT);
        return 1;
    }
    mkdir(DATA_DIR, 0755);

    /* Clean up finished child processes automatically. */
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(server_fd, BACKLOG) < 0) { perror("listen"); return 1; }

    printf("============================================\n");
    printf("  Mbake Kekia portfolio server\n");
    printf("  Serving:  %s\n", web_root_real);
    printf("  Open:     http://localhost:%d\n", port);
    printf("  Stop:     Ctrl+C\n");
    printf("============================================\n");
    fflush(stdout);

    while (running) {
        struct sockaddr_in client;
        socklen_t clen = sizeof client;
        int client_fd = accept(server_fd, (struct sockaddr *)&client, &clen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof ip);

        /* One process per connection so a slow client cannot block others. */
        pid_t pid = fork();
        if (pid == 0) {
            close(server_fd);
            handle_client(client_fd, ip);
            shutdown(client_fd, SHUT_WR);
            close(client_fd);
            _exit(0);
        }
        if (pid < 0) perror("fork");
        close(client_fd);
    }

    printf("\nShutting down. Goodbye!\n");
    close(server_fd);
    return 0;
}
