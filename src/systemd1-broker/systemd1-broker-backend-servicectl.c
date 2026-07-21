/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "systemd1-broker-backend-api.h"

extern char **environ;

#define DEFAULT_SYSVISION_SOCKET "/run/servicectl/sysvision/sysvisiond.sock"
#define DEFAULT_SERVICECTL_SOCKET "/run/servicectl/servicectl.sock"
#define DEFAULT_SERVICECTL_RUNTIME "/run/servicectl/managed"
#define MAX_SYSVISION_RESPONSE_SIZE (1024U * 1024U)
#define MAX_SNAPSHOT_PROPERTIES 256U
#define MAX_JSON_DEPTH 64U
#define SYSVISION_IO_TIMEOUT_SEC 5
#define ACTIVATION_CONNECT_RETRIES 100
#define ACTIVATION_CONNECT_RETRY_NSEC (50U * 1000U * 1000U)

static int write_full(int fd, const char *data, size_t size);

static const char* systemd_unit_path(void) {
        const char *e;

        e = getenv("SYSTEMD1_BROKER_SYSTEMD_UNIT_PATH");
        return e && e[0] ? e : "/etc/systemd/system:/run/systemd/system:/usr/local/lib/systemd/system:/usr/lib/systemd/system";
}

static const char* servicectl_runtime_path(void) {
        const char *e;

        e = getenv("SYSTEMD1_BROKER_SERVICECTL_RUNTIME");
        return e && e[0] ? e : DEFAULT_SERVICECTL_RUNTIME;
}

static bool is_dbus_alias(const char *unit_name) {
        static const char prefix[] = "dbus-";

        return strncmp(unit_name, prefix, sizeof(prefix) - 1) == 0;
}

static int resolve_unit_alias(const char *unit_name, char **ret) {
        char *paths = NULL, *cursor, *directory;
        int r = 0;

        paths = strdup(systemd_unit_path());
        if (!paths)
                return -ENOMEM;

        cursor = paths;
        while ((directory = strsep(&cursor, ":"))) {
                char *canonical = NULL, *path = NULL, *name;
                struct stat st;

                if (!directory[0])
                        continue;
                if (asprintf(&path, "%s/%s", directory, unit_name) < 0) {
                        r = -ENOMEM;
                        break;
                }
                if (lstat(path, &st) < 0) {
                        free(path);
                        if (errno == ENOENT)
                                continue;
                        r = -errno;
                        break;
                }
                if (!S_ISLNK(st.st_mode)) {
                        free(path);
                        break;
                }

                canonical = realpath(path, NULL);
                free(path);
                if (!canonical) {
                        r = -errno;
                        break;
                }

                name = strrchr(canonical, '/');
                name = name ? name + 1 : canonical;
                *ret = strdup(name);
                free(canonical);
                r = *ret ? 1 : -ENOMEM;
                break;
        }

        free(paths);
        return r;
}

static int activate_dbus_unit(const char *unit_name) {
        static const char suffix[] = ".service";
        static const char request[] = "activate\n";
        char *control_path = NULL, response[16];
        const char *runtime;
        struct sockaddr_un sa = {
                .sun_family = AF_UNIX,
        };
        struct timeval timeout = {
                .tv_sec = SYSVISION_IO_TIMEOUT_SEC,
        };
        size_t size;
        ssize_t n;
        int fd = -1, r;

        assert(unit_name);

        size = strlen(unit_name);
        if (size >= sizeof(suffix) - 1 && memcmp(unit_name + size - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) == 0)
                size -= sizeof(suffix) - 1;
        runtime = servicectl_runtime_path();
        if (asprintf(&control_path, "%s/%.*s-dbusd/control.sock", runtime, (int) size, unit_name) < 0)
                return -ENOMEM;
        if (strlen(control_path) >= sizeof(sa.sun_path)) {
                r = -ENAMETOOLONG;
                goto finish;
        }
        strcpy(sa.sun_path, control_path);

        for (unsigned attempt = 0; attempt < ACTIVATION_CONNECT_RETRIES; attempt++) {
                struct timespec delay = {
                        .tv_nsec = ACTIVATION_CONNECT_RETRY_NSEC,
                };

                fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
                if (fd < 0) {
                        r = -errno;
                        goto finish;
                }
                if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
                    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
                        r = -errno;
                        goto finish;
                }
                if (connect(fd, (const struct sockaddr*) &sa, offsetof(struct sockaddr_un, sun_path) + strlen(control_path) + 1) >= 0)
                        break;

                int error = errno;

                r = -error;
                close(fd);
                fd = -1;
                if ((error != ENOENT && error != ECONNREFUSED) || attempt + 1 >= ACTIVATION_CONNECT_RETRIES)
                        goto finish;

                while (nanosleep(&delay, &delay) < 0)
                        if (errno != EINTR) {
                                r = -errno;
                                goto finish;
                        }
        }

        r = write_full(fd, request, sizeof(request) - 1);
        if (r < 0)
                goto finish;
        n = read(fd, response, sizeof(response) - 1);
        if (n < 0) {
                r = -errno;
                goto finish;
        }
        response[n] = 0;
        r = strcmp(response, "ok\n") == 0 ? 0 : -EIO;

finish:
        if (fd >= 0)
                close(fd);
        free(control_path);
        return r;
}

static const char* sysvision_socket_path(void) {
        const char *e;

        e = getenv("SYSTEMD1_BROKER_SYSVISION_SOCKET");
        return e && e[0] ? e : DEFAULT_SYSVISION_SOCKET;
}

static const char* servicectl_socket_path(void) {
        const char *e;

        e = getenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET");
        return e && e[0] ? e : DEFAULT_SERVICECTL_SOCKET;
}

static int connect_unix(const char *path) {
        struct sockaddr_un sa = {
                .sun_family = AF_UNIX,
        };
        struct timeval timeout = {
                .tv_sec = SYSVISION_IO_TIMEOUT_SEC,
        };
        int fd;

        if (strlen(path) >= sizeof(sa.sun_path))
                return -ENAMETOOLONG;
        strcpy(sa.sun_path, path);

        fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
        if (fd < 0)
                return -errno;
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
                int r = -errno;

                close(fd);
                return r;
        }
        if (connect(fd, (const struct sockaddr*) &sa, offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1) < 0) {
                int r = -errno;

                close(fd);
                return r;
        }

        return fd;
}

static int write_full(int fd, const char *data, size_t size) {
        while (size > 0) {
                ssize_t n;

                n = send(fd, data, size, MSG_NOSIGNAL);
                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        return -errno;
                }
                if (n == 0)
                        return -EIO;

                data += n;
                size -= (size_t) n;
        }

        return 0;
}

static int read_all(int fd, char **ret) {
        char *buffer = NULL;
        size_t allocated = 0, size = 0;

        for (;;) {
                ssize_t n;

                if (allocated - size < 4096 && allocated < MAX_SYSVISION_RESPONSE_SIZE + 2) {
                        char *p;
                        size_t new_allocated;

                        new_allocated = allocated > 0 ? allocated * 2 : 8192;
                        if (new_allocated > MAX_SYSVISION_RESPONSE_SIZE + 2)
                                new_allocated = MAX_SYSVISION_RESPONSE_SIZE + 2;
                        p = realloc(buffer, new_allocated);
                        if (!p) {
                                free(buffer);
                                return -ENOMEM;
                        }
                        buffer = p;
                        allocated = new_allocated;
                }

                n = read(fd, buffer + size, allocated - size - 1);
                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        free(buffer);
                        return -errno;
                }
                if (n == 0)
                        break;

                size += (size_t) n;
                if (size > MAX_SYSVISION_RESPONSE_SIZE) {
                        free(buffer);
                        return -E2BIG;
                }
        }

        if (!buffer) {
                buffer = malloc(1);
                if (!buffer)
                        return -ENOMEM;
        }
        buffer[size] = 0;
        *ret = buffer;
        return 0;
}

static int unit_query_name(const char *unit_name, char **ret) {
        static const char suffix[] = ".service";
        size_t size;
        char *name;

        size = strlen(unit_name);
        if (size >= sizeof(suffix) - 1 && memcmp(unit_name + size - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) == 0)
                size -= sizeof(suffix) - 1;

        name = strndup(unit_name, size);
        if (!name)
                return -ENOMEM;

        *ret = name;
        return 0;
}

static int query_sysvision(const char *unit_name, char **ret_body) {
        char *name = NULL, *request = NULL, *response = NULL, *body;
        int fd = -1, status, r;

        r = unit_query_name(unit_name, &name);
        if (r < 0)
                return r;

        if (asprintf(&request,
                     "GET /v1/query/unit/%s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                     name) < 0) {
                r = -ENOMEM;
                goto finish;
        }

        fd = connect_unix(sysvision_socket_path());
        if (fd < 0) {
                r = fd;
                goto finish;
        }

        r = write_full(fd, request, strlen(request));
        if (r < 0)
                goto finish;
        if (shutdown(fd, SHUT_WR) < 0) {
                r = -errno;
                goto finish;
        }

        r = read_all(fd, &response);
        if (r < 0)
                goto finish;
        if (sscanf(response, "HTTP/%*u.%*u %d", &status) != 1) {
                r = -EBADMSG;
                goto finish;
        }
        if (status == 404) {
                r = 0;
                goto finish;
        }
        if (status != 200) {
                r = -EIO;
                goto finish;
        }

        body = strstr(response, "\r\n\r\n");
        if (!body) {
                r = -EBADMSG;
                goto finish;
        }
        body += 4;

        *ret_body = strdup(body);
        r = *ret_body ? 1 : -ENOMEM;

finish:
        if (fd >= 0)
                close(fd);
        free(name);
        free(request);
        free(response);
        return r;
}

static int query_servicectl_units(char **ret_body) {
        static const char request[] =
                "GET /v1/units?all=1 HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Connection: close\r\n\r\n";
        char *response = NULL, *body;
        int fd = -1, status, r;

        fd = connect_unix(servicectl_socket_path());
        if (fd < 0)
                return fd;

        r = write_full(fd, request, sizeof(request) - 1);
        if (r < 0)
                goto finish;
        if (shutdown(fd, SHUT_WR) < 0) {
                r = -errno;
                goto finish;
        }

        r = read_all(fd, &response);
        if (r < 0)
                goto finish;
        if (sscanf(response, "HTTP/%*u.%*u %d", &status) != 1) {
                r = -EBADMSG;
                goto finish;
        }
        if (status != 200) {
                r = -EIO;
                goto finish;
        }

        body = strstr(response, "\r\n\r\n");
        if (!body) {
                r = -EBADMSG;
                goto finish;
        }
        body += 4;
        *ret_body = strdup(body);
        r = *ret_body ? 0 : -ENOMEM;

finish:
        close(fd);
        free(response);
        return r;
}

static int url_encode_path_segment(const char *value, char **ret) {
        static const char hex[] = "0123456789ABCDEF";
        char *encoded, *p;
        size_t size;

        if (strlen(value) > (SIZE_MAX - 1) / 3)
                return -E2BIG;
        size = strlen(value) * 3 + 1;
        encoded = malloc(size);
        if (!encoded)
                return -ENOMEM;

        p = encoded;
        for (const unsigned char *q = (const unsigned char*) value; *q; q++) {
                if ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || strchr("-._~", *q))
                        *(p++) = (char) *q;
                else {
                        *(p++) = '%';
                        *(p++) = hex[*q >> 4];
                        *(p++) = hex[*q & 0x0f];
                }
        }
        *p = 0;
        *ret = encoded;
        return 0;
}

static int query_servicectl_unit(const char *unit_name, char **ret_body) {
        char *encoded = NULL, *request = NULL, *response = NULL, *body;
        int fd = -1, status, r;

        r = url_encode_path_segment(unit_name, &encoded);
        if (r < 0)
                return r;
        if (asprintf(&request,
                     "GET /v1/units/%s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                     encoded) < 0) {
                r = -ENOMEM;
                goto finish;
        }

        fd = connect_unix(servicectl_socket_path());
        if (fd < 0) {
                r = fd;
                goto finish;
        }
        r = write_full(fd, request, strlen(request));
        if (r < 0)
                goto finish;
        if (shutdown(fd, SHUT_WR) < 0) {
                r = -errno;
                goto finish;
        }

        r = read_all(fd, &response);
        if (r < 0)
                goto finish;
        if (sscanf(response, "HTTP/%*u.%*u %d", &status) != 1) {
                r = -EBADMSG;
                goto finish;
        }
        if (status == 404) {
                r = 0;
                goto finish;
        }
        if (status != 200) {
                r = -EIO;
                goto finish;
        }

        body = strstr(response, "\r\n\r\n");
        if (!body) {
                r = -EBADMSG;
                goto finish;
        }
        body += 4;
        *ret_body = strdup(body);
        r = *ret_body ? 1 : -ENOMEM;

finish:
        if (fd >= 0)
                close(fd);
        free(encoded);
        free(request);
        free(response);
        return r;
}

static const char* json_skip_space_bound(const char *p, const char *end) {
        while (p < end && strchr(" \t\r\n", *p))
                p++;
        return p;
}

static bool ascii_is_digit(char c) {
        return c >= '0' && c <= '9';
}

static bool ascii_is_hex(char c) {
        return ascii_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int json_scan_value(const char *p, const char *end, unsigned depth, const char **ret_end);

static int json_scan_string(const char *p, const char *end, const char **ret_end) {
        if (p >= end || *p++ != '"')
                return -EBADMSG;

        while (p < end) {
                unsigned char c = (unsigned char) *(p++);

                if (c == '"') {
                        *ret_end = p;
                        return 0;
                }
                if (c < 0x20)
                        return -EBADMSG;
                if (c != '\\')
                        continue;
                if (p >= end)
                        return -EBADMSG;
                c = (unsigned char) *(p++);
                if (strchr("\"\\/bfnrt", c))
                        continue;
                if (c != 'u' || end - p < 4)
                        return -EBADMSG;
                for (unsigned i = 0; i < 4; i++)
                        if (!ascii_is_hex(p[i]))
                                return -EBADMSG;
                p += 4;
        }

        return -EBADMSG;
}

static int json_scan_number(const char *p, const char *end, const char **ret_end) {
        const char *start = p;

        if (p < end && *p == '-')
                p++;
        if (p >= end)
                return -EBADMSG;
        if (*p == '0')
                p++;
        else {
                if (!ascii_is_digit(*p))
                        return -EBADMSG;
                while (p < end && ascii_is_digit(*p))
                        p++;
        }
        if (p < end && *p == '.') {
                p++;
                if (p >= end || !ascii_is_digit(*p))
                        return -EBADMSG;
                while (p < end && ascii_is_digit(*p))
                        p++;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
                p++;
                if (p < end && (*p == '+' || *p == '-'))
                        p++;
                if (p >= end || !ascii_is_digit(*p))
                        return -EBADMSG;
                while (p < end && ascii_is_digit(*p))
                        p++;
        }
        if (p == start)
                return -EBADMSG;
        *ret_end = p;
        return 0;
}

static int json_scan_value(const char *p, const char *end, unsigned depth, const char **ret_end) {
        int r;

        if (depth > MAX_JSON_DEPTH)
                return -E2BIG;
        p = json_skip_space_bound(p, end);
        if (p >= end)
                return -EBADMSG;
        if (*p == '"')
                return json_scan_string(p, end, ret_end);
        if (*p == '{') {
                p = json_skip_space_bound(p + 1, end);
                if (p < end && *p == '}') {
                        *ret_end = p + 1;
                        return 0;
                }
                for (;;) {
                        r = json_scan_string(p, end, &p);
                        if (r < 0)
                                return r;
                        p = json_skip_space_bound(p, end);
                        if (p >= end || *p++ != ':')
                                return -EBADMSG;
                        r = json_scan_value(p, end, depth + 1, &p);
                        if (r < 0)
                                return r;
                        p = json_skip_space_bound(p, end);
                        if (p >= end)
                                return -EBADMSG;
                        if (*p == '}') {
                                *ret_end = p + 1;
                                return 0;
                        }
                        if (*p++ != ',')
                                return -EBADMSG;
                        p = json_skip_space_bound(p, end);
                }
        }
        if (*p == '[') {
                p = json_skip_space_bound(p + 1, end);
                if (p < end && *p == ']') {
                        *ret_end = p + 1;
                        return 0;
                }
                for (;;) {
                        r = json_scan_value(p, end, depth + 1, &p);
                        if (r < 0)
                                return r;
                        p = json_skip_space_bound(p, end);
                        if (p >= end)
                                return -EBADMSG;
                        if (*p == ']') {
                                *ret_end = p + 1;
                                return 0;
                        }
                        if (*p++ != ',')
                                return -EBADMSG;
                        p = json_skip_space_bound(p, end);
                }
        }
        if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
                *ret_end = p + 4;
                return 0;
        }
        if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
                *ret_end = p + 5;
                return 0;
        }
        if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
                *ret_end = p + 4;
                return 0;
        }
        return json_scan_number(p, end, ret_end);
}

static int json_object_field(
                const char *object,
                const char *object_end,
                const char *field,
                const char **ret_begin,
                const char **ret_end) {

        const char *p, *key_begin, *key_end, *value_begin, *value_end;
        size_t field_size = strlen(field);
        int r;

        p = json_skip_space_bound(object, object_end);
        if (p >= object_end || *p++ != '{')
                return -EBADMSG;
        p = json_skip_space_bound(p, object_end);
        if (p < object_end && *p == '}')
                return 0;

        for (;;) {
                key_begin = p;
                r = json_scan_string(p, object_end, &key_end);
                if (r < 0)
                        return r;
                p = json_skip_space_bound(key_end, object_end);
                if (p >= object_end || *p++ != ':')
                        return -EBADMSG;
                value_begin = json_skip_space_bound(p, object_end);
                r = json_scan_value(value_begin, object_end, 1, &value_end);
                if (r < 0)
                        return r;

                if ((size_t) (key_end - key_begin) == field_size + 2 &&
                    memcmp(key_begin + 1, field, field_size) == 0) {
                        *ret_begin = value_begin;
                        *ret_end = value_end;
                        return 1;
                }

                p = json_skip_space_bound(value_end, object_end);
                if (p >= object_end)
                        return -EBADMSG;
                if (*p == '}')
                        return 0;
                if (*p++ != ',')
                        return -EBADMSG;
                p = json_skip_space_bound(p, object_end);
        }
}

static unsigned json_hex_value(char c) {
        if (c >= '0' && c <= '9')
                return (unsigned) (c - '0');
        if (c >= 'a' && c <= 'f')
                return (unsigned) (c - 'a' + 10);
        return (unsigned) (c - 'A' + 10);
}

static uint32_t json_unicode_escape(const char *p) {
        return (json_hex_value(p[0]) << 12) |
               (json_hex_value(p[1]) << 8) |
               (json_hex_value(p[2]) << 4) |
               json_hex_value(p[3]);
}

static int utf8_append(char **p, uint32_t codepoint) {
        if (codepoint <= 0x7f)
                *((*p)++) = (char) codepoint;
        else if (codepoint <= 0x7ff) {
                *((*p)++) = (char) (0xc0 | codepoint >> 6);
                *((*p)++) = (char) (0x80 | (codepoint & 0x3f));
        } else if (codepoint <= 0xffff) {
                *((*p)++) = (char) (0xe0 | codepoint >> 12);
                *((*p)++) = (char) (0x80 | ((codepoint >> 6) & 0x3f));
                *((*p)++) = (char) (0x80 | (codepoint & 0x3f));
        } else {
                *((*p)++) = (char) (0xf0 | codepoint >> 18);
                *((*p)++) = (char) (0x80 | ((codepoint >> 12) & 0x3f));
                *((*p)++) = (char) (0x80 | ((codepoint >> 6) & 0x3f));
                *((*p)++) = (char) (0x80 | (codepoint & 0x3f));
        }
        return 0;
}

static int json_decode_string(const char *begin, const char *end, char **ret) {
        char *decoded, *q;
        const char *p;

        if (begin >= end || *begin != '"' || end[-1] != '"')
                return -EBADMSG;
        decoded = malloc((size_t) (end - begin));
        if (!decoded)
                return -ENOMEM;

        q = decoded;
        for (p = begin + 1; p < end - 1; p++) {
                uint32_t codepoint;

                if (*p != '\\') {
                        *(q++) = *p;
                        continue;
                }
                p++;
                switch (*p) {
                case '"': case '\\': case '/': *(q++) = *p; break;
                case 'b': *(q++) = '\b'; break;
                case 'f': *(q++) = '\f'; break;
                case 'n': *(q++) = '\n'; break;
                case 'r': *(q++) = '\r'; break;
                case 't': *(q++) = '\t'; break;
                case 'u':
                        codepoint = json_unicode_escape(p + 1);
                        p += 4;
                        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                                uint32_t low;

                                if (end - p < 7 || p[1] != '\\' || p[2] != 'u') {
                                        free(decoded);
                                        return -EBADMSG;
                                }
                                low = json_unicode_escape(p + 3);
                                if (low < 0xdc00 || low > 0xdfff) {
                                        free(decoded);
                                        return -EBADMSG;
                                }
                                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                                p += 6;
                        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                                free(decoded);
                                return -EBADMSG;
                        }
                        utf8_append(&q, codepoint);
                        break;
                default:
                        free(decoded);
                        return -EBADMSG;
                }
        }
        *q = 0;
        *ret = decoded;
        return 0;
}

static int json_object_string_field(const char *object, const char *object_end, const char *field, char **ret) {
        const char *begin, *end;
        int r;

        r = json_object_field(object, object_end, field, &begin, &end);
        if (r <= 0)
                return r;
        r = json_decode_string(begin, end, ret);
        return r < 0 ? r : 1;
}

static int json_validate_document(const char *json, const char **ret_end) {
        const char *end, *p;
        int r;

        end = json + strlen(json);
        r = json_scan_value(json, end, 0, &p);
        if (r < 0)
                return r;
        p = json_skip_space_bound(p, end);
        if (p != end)
                return -EBADMSG;
        if (ret_end)
                *ret_end = p;
        return 0;
}

static int snapshot_property_append(
                Systemd1BrokerBackendProperty **properties,
                size_t *n_properties,
                const char *object,
                const char *object_end) {

        Systemd1BrokerBackendProperty property = {
                .size = sizeof(Systemd1BrokerBackendProperty),
        };
        Systemd1BrokerBackendProperty *p;
        const char *value_begin, *value_end;
        int r;

        if (*n_properties >= MAX_SNAPSHOT_PROPERTIES)
                return -E2BIG;

        r = json_object_string_field(object, object_end, "interface", (char**) &property.interface);
        if (r <= 0)
                return r < 0 ? r : -EBADMSG;
        r = json_object_string_field(object, object_end, "name", (char**) &property.name);
        if (r <= 0) {
                r = r < 0 ? r : -EBADMSG;
                goto fail;
        }
        r = json_object_string_field(object, object_end, "signature", (char**) &property.signature);
        if (r <= 0) {
                r = r < 0 ? r : -EBADMSG;
                goto fail;
        }
        r = json_object_field(object, object_end, "value", &value_begin, &value_end);
        if (r <= 0) {
                r = r < 0 ? r : -EBADMSG;
                goto fail;
        }
        property.value_json = strndup(value_begin, (size_t) (value_end - value_begin));
        if (!property.value_json) {
                r = -ENOMEM;
                goto fail;
        }
        if (!property.interface[0] || !property.name[0] || !property.signature[0]) {
                r = -EBADMSG;
                goto fail;
        }

        p = realloc(*properties, (*n_properties + 1) * sizeof(Systemd1BrokerBackendProperty));
        if (!p) {
                r = -ENOMEM;
                goto fail;
        }
        *properties = p;
        (*properties)[(*n_properties)++] = property;
        return 0;

fail:
        free((char*) property.interface);
        free((char*) property.name);
        free((char*) property.signature);
        free((char*) property.value_json);
        return r;
}

static int snapshot_properties_parse(
                const char *array_begin,
                const char *array_end,
                Systemd1BrokerBackendProperty **ret_properties,
                size_t *ret_n_properties) {

        Systemd1BrokerBackendProperty *properties = NULL;
        const char *p, *value_end;
        size_t n_properties = 0;
        int r;

        *ret_properties = NULL;
        *ret_n_properties = 0;

        p = json_skip_space_bound(array_begin, array_end);
        if (p >= array_end || *p++ != '[')
                return -EBADMSG;
        p = json_skip_space_bound(p, array_end);
        if (p < array_end && *p == ']')
                goto success;

        for (;;) {
                p = json_skip_space_bound(p, array_end);
                if (p >= array_end || *p != '{') {
                        r = -EBADMSG;
                        goto fail;
                }
                r = json_scan_value(p, array_end, 1, &value_end);
                if (r < 0)
                        goto fail;
                r = snapshot_property_append(&properties, &n_properties, p, value_end);
                if (r < 0)
                        goto fail;
                p = json_skip_space_bound(value_end, array_end);
                if (p >= array_end) {
                        r = -EBADMSG;
                        goto fail;
                }
                if (*p == ']')
                        break;
                if (*p++ != ',') {
                        r = -EBADMSG;
                        goto fail;
                }
        }

success:
        *ret_properties = properties;
        *ret_n_properties = n_properties;
        return 0;

fail:
        for (size_t i = 0; i < n_properties; i++) {
                free((char*) properties[i].interface);
                free((char*) properties[i].name);
                free((char*) properties[i].signature);
                free((char*) properties[i].value_json);
        }
        free(properties);
        *ret_properties = NULL;
        *ret_n_properties = 0;
        return r;
}

static int json_string(const char *json, const char *field, char **ret) {
        char *pattern = NULL, *value = NULL;
        const char *p, *q;
        int r = 0;

        if (asprintf(&pattern, "\"%s\"", field) < 0)
                return -ENOMEM;

        p = strstr(json, pattern);
        if (!p)
                goto finish;
        p += strlen(pattern);
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
        if (*p++ != ':') {
                r = -EBADMSG;
                goto finish;
        }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
        if (*p++ != '"') {
                r = -EBADMSG;
                goto finish;
        }

        q = p;
        while (*q && *q != '"') {
                if (*q == '\\' && q[1])
                        q += 2;
                else
                        q++;
        }
        if (*q != '"') {
                r = -EBADMSG;
                goto finish;
        }

        value = strndup(p, (size_t) (q - p));
        if (!value) {
                r = -ENOMEM;
                goto finish;
        }

        *ret = value;
        value = NULL;
        r = 1;

finish:
        free(pattern);
        free(value);
        return r;
}

static bool valid_unit_id(const char *id) {
        size_t n;

        if (!id || !id[0] || strchr(id, '/') || strchr(id, '\\') || strchr(id, ' '))
                return false;
        n = strlen(id);
        return n > strlen(".service") && strcmp(id + n - strlen(".service"), ".service") == 0;
}

static int catalog_unit_id(const char *name, char **ret) {
        static const char suffix[] = ".service";
        char *id;
        size_t n;

        if (!name || !name[0])
                return -EINVAL;
        n = strlen(name);
        if (n >= sizeof(suffix) - 1 && memcmp(name + n - (sizeof(suffix) - 1), suffix, sizeof(suffix) - 1) == 0)
                id = strdup(name);
        else if (asprintf(&id, "%s.service", name) < 0)
                id = NULL;
        if (!id)
                return -ENOMEM;
        if (!valid_unit_id(id)) {
                free(id);
                return -EINVAL;
        }

        *ret = id;
        return 0;
}

static const char* skip_json_space(const char *p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                p++;
        return p;
}

static int find_units_array(const char *body, const char **ret_begin, const char **ret_end) {
        const char *p, *begin;
        bool quoted = false, escaped = false;
        unsigned depth = 0;

        p = strstr(body, "\"units\"");
        if (!p)
                return -EBADMSG;
        p = skip_json_space(p + strlen("\"units\""));
        if (*p++ != ':')
                return -EBADMSG;
        p = skip_json_space(p);
        if (*p++ != '[')
                return -EBADMSG;
        begin = p;

        for (; *p; p++) {
                if (quoted) {
                        if (escaped)
                                escaped = false;
                        else if (*p == '\\')
                                escaped = true;
                        else if (*p == '"')
                                quoted = false;
                        continue;
                }
                if (*p == '"')
                        quoted = true;
                else if (*p == '[' || *p == '{')
                        depth++;
                else if (*p == '}') {
                        if (depth == 0)
                                return -EBADMSG;
                        depth--;
                } else if (*p == ']') {
                        if (depth == 0) {
                                *ret_begin = begin;
                                *ret_end = p;
                                return 0;
                        }
                        depth--;
                }
        }

        return -EBADMSG;
}

static int next_catalog_object(const char **cursor, const char *end, char **ret) {
        const char *p, *begin;
        bool quoted = false, escaped = false;
        unsigned depth = 0;

        p = skip_json_space(*cursor);
        if (p >= end)
                return 0;
        if (*p == ',')
                p = skip_json_space(p + 1);
        if (p >= end || *p != '{')
                return -EBADMSG;
        begin = p++;
        depth = 1;

        for (; p < end; p++) {
                if (quoted) {
                        if (escaped)
                                escaped = false;
                        else if (*p == '\\')
                                escaped = true;
                        else if (*p == '"')
                                quoted = false;
                        continue;
                }
                if (*p == '"')
                        quoted = true;
                else if (*p == '{')
                        depth++;
                else if (*p == '}' && --depth == 0) {
                        *ret = strndup(begin, (size_t) (p - begin + 1));
                        if (!*ret)
                                return -ENOMEM;
                        *cursor = p + 1;
                        return 1;
                }
        }

        return -EBADMSG;
}

static bool string_equal(const char *a, const char *b) {
        return a && strcasecmp(a, b) == 0;
}

static bool string_startswith(const char *a, const char *prefix) {
        return a && strncasecmp(a, prefix, strlen(prefix)) == 0;
}

static bool state_has_failed_exit(const char *state) {
        static const char marker[] = "EXITED - STATUS ";
        const char *p;
        char *end;
        long status;

        if (!state)
                return false;

        p = strcasestr(state, marker);
        if (!p)
                return false;

        errno = 0;
        status = strtol(p + strlen(marker), &end, 10);
        return errno != 0 || end == p + strlen(marker) || status != 0;
}

static int state_from_snapshot(const char *body, Systemd1BrokerBackendState *ret) {
        char *state = NULL, *lifecycle = NULL, *phase = NULL, *child_state = NULL, *failure = NULL;
        Systemd1BrokerBackendState result;
        int r;

        r = json_string(body, "state", &state);
        if (r <= 0)
                return r < 0 ? r : -EBADMSG;
        (void) json_string(body, "lifecycle", &lifecycle);
        (void) json_string(body, "phase", &phase);
        (void) json_string(body, "child_state", &child_state);
        (void) json_string(body, "failure", &failure);

        if ((failure && failure[0]) || string_equal(lifecycle, "failed") || state_has_failed_exit(state))
                result = SYSTEMD1_BROKER_BACKEND_FAILED;
        else if (string_equal(lifecycle, "activating") || string_equal(phase, "starting") || string_equal(child_state, "starting"))
                result = SYSTEMD1_BROKER_BACKEND_STARTING;
        else if (string_equal(lifecycle, "deactivating") || string_equal(phase, "stopping") || string_equal(child_state, "stopping"))
                result = SYSTEMD1_BROKER_BACKEND_STOPPING;
        else if (string_equal(lifecycle, "active") || string_equal(lifecycle, "running") || string_equal(state, "STARTED"))
                result = SYSTEMD1_BROKER_BACKEND_RUNNING;
        else if (string_equal(lifecycle, "inactive") || string_equal(lifecycle, "stopped") || string_startswith(state, "STOPPED") || string_equal(state, "NOT LOADED"))
                result = SYSTEMD1_BROKER_BACKEND_STOPPED;
        else
                result = SYSTEMD1_BROKER_BACKEND_ABSENT;

        free(state);
        free(lifecycle);
        free(phase);
        free(child_state);
        free(failure);
        *ret = result;
        return 0;
}

static void servicectl_free_unit_snapshot(void *userdata, Systemd1BrokerBackendUnitSnapshot *snapshot) {
        Systemd1BrokerBackendProperty *properties;

        if (!snapshot)
                return;

        properties = (Systemd1BrokerBackendProperty*) snapshot->properties;
        for (size_t i = 0; i < snapshot->n_properties; i++) {
                free((char*) properties[i].interface);
                free((char*) properties[i].name);
                free((char*) properties[i].signature);
                free((char*) properties[i].value_json);
        }
        free(properties);
        free((char*) snapshot->description);
        free(snapshot);
}

static int servicectl_get_unit_snapshot(
                void *userdata,
                const char *unit_name,
                const Systemd1BrokerBackendUnitExtra *extra,
                Systemd1BrokerBackendUnitSnapshot **ret_snapshot) {

        Systemd1BrokerBackendUnitSnapshot *snapshot = NULL;
        Systemd1BrokerBackendProperty *properties = NULL;
        const char *body_end, *properties_begin, *properties_end, *unit_begin, *unit_end;
        char *body = NULL, *unit_json = NULL;
        size_t n_properties = 0;
        int r;

        if (!unit_name || !extra || extra->size < sizeof(Systemd1BrokerBackendUnitExtra) || !ret_snapshot)
                return -EINVAL;
        *ret_snapshot = NULL;

        r = query_servicectl_unit(unit_name, &body);
        if (r < 0)
                return r;

        snapshot = calloc(1, sizeof(Systemd1BrokerBackendUnitSnapshot));
        if (!snapshot) {
                free(body);
                return -ENOMEM;
        }
        snapshot->size = sizeof(Systemd1BrokerBackendUnitSnapshot);
        if (r == 0) {
                snapshot->state = SYSTEMD1_BROKER_BACKEND_ABSENT;
                *ret_snapshot = snapshot;
                return 0;
        }

        r = json_validate_document(body, &body_end);
        if (r < 0)
                goto fail;
        r = json_object_field(body, body_end, "unit", &unit_begin, &unit_end);
        if (r <= 0) {
                r = r < 0 ? r : -EBADMSG;
                goto fail;
        }
        unit_json = strndup(unit_begin, (size_t) (unit_end - unit_begin));
        if (!unit_json) {
                r = -ENOMEM;
                goto fail;
        }
        r = state_from_snapshot(unit_json, &snapshot->state);
        if (r < 0)
                goto fail;
        r = json_object_string_field(unit_begin, unit_end, "description", (char**) &snapshot->description);
        if (r < 0)
                goto fail;

        r = json_object_field(body, body_end, "systemd_properties", &properties_begin, &properties_end);
        if (r <= 0) {
                r = r < 0 ? r : -EBADMSG;
                goto fail;
        }
        r = snapshot_properties_parse(properties_begin, properties_end, &properties, &n_properties);
        if (r < 0)
                goto fail;
        snapshot->properties = properties;
        snapshot->n_properties = n_properties;

        free(unit_json);
        free(body);
        *ret_snapshot = snapshot;
        return 0;

fail:
        free(unit_json);
        free(body);
        snapshot->properties = properties;
        snapshot->n_properties = n_properties;
        servicectl_free_unit_snapshot(NULL, snapshot);
        return r;
}

static void servicectl_free_units(void *userdata, Systemd1BrokerBackendUnit *units, size_t n_units) {
        if (!units)
                return;

        for (size_t i = 0; i < n_units; i++) {
                free((char*) units[i].id);
                free((char*) units[i].description);
        }
        free(units);
}

static int servicectl_list_units(void *userdata, Systemd1BrokerBackendUnit **ret_units, size_t *ret_n_units) {
        Systemd1BrokerBackendUnit *units = NULL;
        char *body = NULL;
        const char *cursor, *end;
        size_t n_units = 0;
        int r;

        if (!ret_units || !ret_n_units)
                return -EINVAL;

        r = query_servicectl_units(&body);
        if (r < 0)
                return r;
        r = find_units_array(body, &cursor, &end);
        if (r < 0)
                goto fail;

        for (;;) {
                Systemd1BrokerBackendUnit unit = {
                        .size = sizeof(Systemd1BrokerBackendUnit),
                };
                Systemd1BrokerBackendUnit *p;
                char *description = NULL, *name = NULL, *object = NULL;

                r = next_catalog_object(&cursor, end, &object);
                if (r <= 0) {
                        free(object);
                        if (r < 0)
                                goto fail;
                        break;
                }

                r = json_string(object, "name", &name);
                if (r <= 0) {
                        r = r < 0 ? r : -EBADMSG;
                        free(object);
                        free(name);
                        goto fail;
                }
                r = catalog_unit_id(name, (char**) &unit.id);
                free(name);
                if (r < 0) {
                        free(object);
                        goto fail;
                }
                for (size_t i = 0; i < n_units; i++)
                        if (strcmp(units[i].id, unit.id) == 0) {
                                r = -EEXIST;
                                free(object);
                                free((char*) unit.id);
                                goto fail;
                        }

                r = json_string(object, "description", &description);
                if (r < 0) {
                        free(object);
                        free((char*) unit.id);
                        goto fail;
                }
                if (r == 0 || !description[0]) {
                        free(description);
                        description = strdup(unit.id);
                        if (!description) {
                                r = -ENOMEM;
                                free(object);
                                free((char*) unit.id);
                                goto fail;
                        }
                }
                unit.description = description;

                r = state_from_snapshot(object, &unit.state);
                free(object);
                if (r < 0) {
                        free((char*) unit.id);
                        free(description);
                        goto fail;
                }

                p = realloc(units, (n_units + 1) * sizeof(Systemd1BrokerBackendUnit));
                if (!p) {
                        r = -ENOMEM;
                        free((char*) unit.id);
                        free(description);
                        goto fail;
                }
                units = p;
                units[n_units++] = unit;
        }

        free(body);
        *ret_units = units;
        *ret_n_units = n_units;
        return 0;

fail:
        free(body);
        servicectl_free_units(NULL, units, n_units);
        return r;
}

static int run_servicectl(const char *verb, const char *unit_name) {
        pid_t pid;
        int status;
        char *argv[] = {
                (char*) "servicectl",
                (char*) verb,
                (char*) unit_name,
                NULL,
        };

        int r = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
        if (r != 0)
                return -r;

        if (waitpid(pid, &status, 0) < 0)
                return -errno;

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return 0;

        if (WIFEXITED(status))
                return -EIO;

        return -ECHILD;
}

static int servicectl_status(
                void *userdata,
                const char *unit_name,
                const Systemd1BrokerBackendUnitExtra *extra,
                Systemd1BrokerBackendState *ret_state) {

        char *body = NULL;
        int r;

        if (!unit_name || !extra || extra->size < sizeof(Systemd1BrokerBackendUnitExtra) || !ret_state)
                return -EINVAL;

        r = query_sysvision(unit_name, &body);
        if (r < 0)
                return r;
        if (r == 0) {
                *ret_state = SYSTEMD1_BROKER_BACKEND_ABSENT;
                return 0;
        }

        r = state_from_snapshot(body, ret_state);
        free(body);
        return r;
}

static int servicectl_start(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra) {
        char *canonical = NULL;
        int r;

        if (!unit_name || !extra || extra->size < sizeof(Systemd1BrokerBackendUnitExtra))
                return -EINVAL;

        if (is_dbus_alias(unit_name)) {
                r = resolve_unit_alias(unit_name, &canonical);
                if (r < 0)
                        return r;
                if (r > 0) {
                        r = run_servicectl("start", canonical);
                        if (r >= 0)
                                r = activate_dbus_unit(canonical);
                        free(canonical);
                        return r;
                }
        }

        return run_servicectl("start", unit_name);
}

static int servicectl_stop(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra) {
        if (!unit_name || !extra || extra->size < sizeof(Systemd1BrokerBackendUnitExtra))
                return -EINVAL;

        return run_servicectl("stop", unit_name);
}

__attribute__((visibility("default"))) const Systemd1BrokerBackendOps* systemd1_broker_backend_get_ops(void) {
        static const Systemd1BrokerBackendOps ops = {
                .size = sizeof(Systemd1BrokerBackendOps),
                .status = servicectl_status,
                .start = servicectl_start,
                .stop = servicectl_stop,
                .list_units = servicectl_list_units,
                .free_units = servicectl_free_units,
                .get_unit_snapshot = servicectl_get_unit_snapshot,
                .free_unit_snapshot = servicectl_free_unit_snapshot,
        };

        return &ops;
}
