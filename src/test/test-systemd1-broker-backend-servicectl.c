/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "alloc-util.h"
#include "dlfcn-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "io-util.h"
#include "mkdir.h"
#include "path-util.h"
#include "rm-rf.h"
#include "socket-util.h"
#include "string-util.h"
#include "systemd1-broker-backend-api.h"
#include "tests.h"
#include "time-util.h"
#include "tmpfile-util.h"

typedef struct HTTPStub {
        int listen_fd;
        pthread_t thread;
        const char *response;
        char *request;
} HTTPStub;

static void* http_stub_thread(void *userdata) {
        HTTPStub *stub = ASSERT_PTR(userdata);
        _cleanup_close_ int fd = -EBADF;
        char request[4096];
        ssize_t n;

        ASSERT_OK_POSITIVE(fd_wait_for_event(stub->listen_fd, POLLIN, USEC_PER_SEC));
        fd = accept4(stub->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        ASSERT_OK_ERRNO(fd);

        n = read(fd, request, sizeof(request) - 1);
        ASSERT_OK_POSITIVE(n);
        request[n] = 0;
        stub->request = ASSERT_PTR(strdup(request));

        ASSERT_OK(loop_write(fd, stub->response, strlen(stub->response)));
        return NULL;
}

static void http_stub_start(HTTPStub *stub, const char *path, const char *response) {
        union sockaddr_union sa;

        assert(stub);
        assert(path);
        assert(response);

        *stub = (HTTPStub) {
                .listen_fd = -EBADF,
                .response = response,
        };

        ASSERT_OK(stub->listen_fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0));
        ASSERT_OK(sockaddr_un_set_path(&sa.un, path));
        ASSERT_OK_ERRNO(bind(stub->listen_fd, &sa.sa, sockaddr_un_len(&sa.un)));
        ASSERT_OK_ERRNO(listen(stub->listen_fd, 1));
        ASSERT_OK(-pthread_create(&stub->thread, NULL, http_stub_thread, stub));
}

static void http_stub_done(HTTPStub *stub) {
        ASSERT_OK(-pthread_join(stub->thread, NULL));
        stub->listen_fd = safe_close(stub->listen_fd);
}

static const Systemd1BrokerBackendOps* load_backend(void **ret_dl) {
        const Systemd1BrokerBackendOps* (*entrypoint)(void);
        const Systemd1BrokerBackendOps *ops;
        const char *build_root, *library_path;
        void *dl;

        assert(ret_dl);

        build_root = ASSERT_PTR(getenv("PROJECT_BUILD_ROOT"));
        library_path = strjoina(build_root, "/src/systemd1-broker/libsystemd1-broker-backend-servicectl.so");
        dl = ASSERT_PTR(dlopen(library_path, RTLD_NOW|RTLD_LOCAL));
        entrypoint = (typeof(entrypoint)) ASSERT_PTR(dlsym(dl, "systemd1_broker_backend_get_ops"));
        ops = ASSERT_PTR(entrypoint());

        ASSERT_EQ(ops->size, sizeof(Systemd1BrokerBackendOps));
        ASSERT_NOT_NULL(ops->status);
        ASSERT_NOT_NULL(ops->start);
        ASSERT_NOT_NULL(ops->stop);
        ASSERT_NOT_NULL(ops->list_units);
        ASSERT_NOT_NULL(ops->free_units);
        ASSERT_NULL(dlsym(dl, "sd_bus_open_system"));

        *ret_dl = dl;
        return ops;
}

TEST(list_units_uses_servicectl_api) {
        static const char body[] =
                "{\"generated_at\":\"2026-07-17T00:00:00Z\",\"units\":["
                "{\"name\":\"demo.worker\",\"description\":\"Demo\",\"state\":\"STARTED\",\"lifecycle\":\"ready\"},"
                "{\"name\":\"stopped.service\",\"description\":\"Stopped\",\"state\":\"STOPPED\",\"lifecycle\":\"stopped\"}]}";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *response = NULL, *socket_path = NULL;
        Systemd1BrokerBackendUnit *units = NULL;
        const Systemd1BrokerBackendOps *ops;
        HTTPStub stub;
        size_t n_units = 0;

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "servicectl.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET", socket_path, true));
        ASSERT_OK(asprintf(&response,
                           "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           strlen(body), body));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_OK(ops->list_units(ops->userdata, &units, &n_units));
        http_stub_done(&stub);

        ASSERT_EQ(n_units, 2u);
        ASSERT_EQ(units[0].size, sizeof(Systemd1BrokerBackendUnit));
        ASSERT_STREQ(units[0].id, "demo.worker.service");
        ASSERT_STREQ(units[0].description, "Demo");
        ASSERT_EQ(units[0].state, SYSTEMD1_BROKER_BACKEND_RUNNING);
        ASSERT_STREQ(units[1].id, "stopped.service");
        ASSERT_STREQ(units[1].description, "Stopped");
        ASSERT_EQ(units[1].state, SYSTEMD1_BROKER_BACKEND_STOPPED);
        ASSERT_TRUE(startswith(stub.request, "GET /v1/units?all=1 HTTP/1.1\r\n"));

        ops->free_units(ops->userdata, units, n_units);
        free(stub.request);
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET"));
}

TEST(list_units_rejects_duplicate_catalog) {
        static const char body[] =
                "{\"units\":["
                "{\"name\":\"demo\",\"state\":\"STARTED\"},"
                "{\"name\":\"demo.service\",\"state\":\"STOPPED\"}]}";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *response = NULL, *socket_path = NULL;
        Systemd1BrokerBackendUnit *units = NULL;
        const Systemd1BrokerBackendOps *ops;
        HTTPStub stub;
        size_t n_units = 0;

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "servicectl.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET", socket_path, true));
        ASSERT_OK(asprintf(&response,
                           "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           strlen(body), body));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_ERROR(ops->list_units(ops->userdata, &units, &n_units), EEXIST);
        http_stub_done(&stub);
        ASSERT_NULL(units);
        ASSERT_EQ(n_units, 0u);

        free(stub.request);
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET"));
}

TEST(list_units_rejects_malformed_catalog) {
        static const char body[] = "{\"units\":[{\"name\":\"demo\"}]}";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *response = NULL, *socket_path = NULL;
        Systemd1BrokerBackendUnit *units = NULL;
        const Systemd1BrokerBackendOps *ops;
        HTTPStub stub;
        size_t n_units = 0;

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "servicectl.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET", socket_path, true));
        ASSERT_OK(asprintf(&response,
                           "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           strlen(body), body));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_ERROR(ops->list_units(ops->userdata, &units, &n_units), EBADMSG);
        http_stub_done(&stub);
        ASSERT_NULL(units);

        free(stub.request);
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET"));
}

TEST(status_uses_sysvision_api) {
        static const struct {
                const char *body;
                Systemd1BrokerBackendState expected;
        } cases[] = {
                { "{\"state\":\"STARTED\",\"lifecycle\":\"ready\"}", SYSTEMD1_BROKER_BACKEND_RUNNING },
                { "{\"state\":\"STARTED\",\"phase\":\"starting\"}", SYSTEMD1_BROKER_BACKEND_STARTING },
                { "{\"state\":\"STARTED\",\"child_state\":\"stopping\"}", SYSTEMD1_BROKER_BACKEND_STOPPING },
                { "{\"state\":\"STOPPED\",\"lifecycle\":\"stopped\"}", SYSTEMD1_BROKER_BACKEND_STOPPED },
                { "{\"state\":\"STOPPED - EXITED - STATUS 0\"}", SYSTEMD1_BROKER_BACKEND_STOPPED },
                { "{\"state\":\"STOPPED - EXITED - STATUS 1\"}", SYSTEMD1_BROKER_BACKEND_FAILED },
                { "{\"state\":\"STARTED\",\"failure\":\"readiness timeout\"}", SYSTEMD1_BROKER_BACKEND_FAILED },
        };
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *socket_path = NULL;
        const Systemd1BrokerBackendOps *ops;
        const Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = "demo.service",
        };

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "sysvisiond.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SYSVISION_SOCKET", socket_path, true));
        ops = load_backend(&dl);

        FOREACH_ELEMENT(test, cases) {
                _cleanup_free_ char *response = NULL;
                HTTPStub stub;
                Systemd1BrokerBackendState state = _SYSTEMD1_BROKER_BACKEND_STATE_INVALID;

                ASSERT_OK(asprintf(&response,
                                   "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                                   strlen(test->body), test->body));
                http_stub_start(&stub, socket_path, response);
                ASSERT_OK(ops->status(ops->userdata, "demo.service", &extra, &state));
                http_stub_done(&stub);

                ASSERT_EQ(state, test->expected);
                ASSERT_TRUE(startswith(stub.request, "GET /v1/query/unit/demo HTTP/1.1\r\n"));
                free(stub.request);
                ASSERT_OK_ERRNO(unlink(socket_path));
        }

        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SYSVISION_SOCKET"));
}

TEST(status_maps_missing_unit_to_absent) {
        static const char response[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *socket_path = NULL;
        const Systemd1BrokerBackendOps *ops;
        const Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = "missing.service",
        };
        HTTPStub stub;
        Systemd1BrokerBackendState state = _SYSTEMD1_BROKER_BACKEND_STATE_INVALID;

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "sysvisiond.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SYSVISION_SOCKET", socket_path, true));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_OK(ops->status(ops->userdata, "missing.service", &extra, &state));
        http_stub_done(&stub);

        ASSERT_EQ(state, SYSTEMD1_BROKER_BACKEND_ABSENT);
        ASSERT_TRUE(startswith(stub.request, "GET /v1/query/unit/missing HTTP/1.1\r\n"));
        free(stub.request);
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SYSVISION_SOCKET"));
}

TEST(status_rejects_snapshot_without_state) {
        static const char body[] = "{\"lifecycle\":\"ready\"}";
        static const char response[] =
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: 21\r\n"
                "Connection: close\r\n\r\n"
                "{\"lifecycle\":\"ready\"}";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *socket_path = NULL;
        const Systemd1BrokerBackendOps *ops;
        const Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = "demo.service",
        };
        HTTPStub stub;
        Systemd1BrokerBackendState state = _SYSTEMD1_BROKER_BACKEND_STATE_INVALID;

        assert_cc(sizeof(body) - 1 == 21);
        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "sysvisiond.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SYSVISION_SOCKET", socket_path, true));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_ERROR(ops->status(ops->userdata, "demo.service", &extra, &state), EBADMSG);
        http_stub_done(&stub);

        free(stub.request);
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SYSVISION_SOCKET"));
}

TEST(start_routes_dbus_alias_through_activation) {
        static const char script[] =
                "#!/bin/sh\n"
                "printf '%s\\n' \"$@\" >\"$SYSTEMD1_BROKER_TEST_ARGS\"\n";
        static const char response[] = "ok\n";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *args = NULL, *args_path = NULL, *canonical_path = NULL, *control_dir = NULL,
                        *control_path = NULL, *runtime_path = NULL, *servicectl_path = NULL, *alias_path = NULL,
                        *old_path = NULL;
        const Systemd1BrokerBackendOps *ops;
        const Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = "dbus-org.freedesktop.locale1.service",
        };
        HTTPStub stub;

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        args_path = path_join(tmp, "args");
        canonical_path = path_join(tmp, "systemd-localed.service");
        runtime_path = path_join(tmp, "managed");
        control_dir = path_join(runtime_path, "systemd-localed-dbusd");
        control_path = path_join(control_dir, "control.sock");
        servicectl_path = path_join(tmp, "servicectl");
        alias_path = path_join(tmp, "dbus-org.freedesktop.locale1.service");
        ASSERT_NOT_NULL(args_path);
        ASSERT_NOT_NULL(canonical_path);
        ASSERT_NOT_NULL(runtime_path);
        ASSERT_NOT_NULL(control_dir);
        ASSERT_NOT_NULL(control_path);
        ASSERT_NOT_NULL(servicectl_path);
        ASSERT_NOT_NULL(alias_path);

        ASSERT_OK(write_string_file(canonical_path, "", WRITE_STRING_FILE_CREATE));
        ASSERT_OK_ERRNO(symlink("systemd-localed.service", alias_path));
        ASSERT_OK(mkdir_p(control_dir, 0755));
        ASSERT_OK(write_string_file(servicectl_path, script, WRITE_STRING_FILE_CREATE));
        ASSERT_OK_ERRNO(chmod(servicectl_path, 0755));

        old_path = strdup(getenv("PATH") ?: "");
        ASSERT_NOT_NULL(old_path);
        ASSERT_OK_ERRNO(setenv("PATH", tmp, true));
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SYSTEMD_UNIT_PATH", tmp, true));
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SERVICECTL_RUNTIME", runtime_path, true));
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_TEST_ARGS", args_path, true));
        ops = load_backend(&dl);

        http_stub_start(&stub, control_path, response);
        ASSERT_OK(ops->start(ops->userdata, extra.id, &extra));
        http_stub_done(&stub);
        ASSERT_OK(read_full_file(args_path, &args, NULL));
        ASSERT_STREQ(args, "start\nsystemd-localed.service\n");
        ASSERT_STREQ(stub.request, "activate\n");
        free(stub.request);

        ASSERT_OK_ERRNO(setenv("PATH", old_path, true));
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SYSTEMD_UNIT_PATH"));
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SERVICECTL_RUNTIME"));
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_TEST_ARGS"));
}

DEFINE_TEST_MAIN(LOG_INFO);
