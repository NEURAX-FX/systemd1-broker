/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
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

static int make_chunked_response(const char *body, size_t split, char **ret) {
        size_t size;

        assert(body);
        assert(ret);

        size = strlen(body);
        split = MIN(split, size);
        if (asprintf(ret,
                     "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                     "%zx\r\n%.*s\r\n%zx\r\n%s\r\n0\r\n\r\n",
                     split, (int) split, body, size - split, body + split) < 0)
                return -ENOMEM;
        return 0;
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
        ASSERT_NOT_NULL(ops->get_unit_snapshot);
        ASSERT_NOT_NULL(ops->free_unit_snapshot);
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
        ASSERT_OK(make_chunked_response(body, 47, &response));
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
        ASSERT_OK(make_chunked_response(body, 83, &response));
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

TEST(get_unit_snapshot_uses_servicectl_api) {
        static const char body[] =
                "{\"unit\":{\"name\":\"demo.service\",\"description\":\"Demo \\u0026 Worker\","
                "\"state\":\"STARTED\",\"lifecycle\":\"ready\"},"
                "\"systemd_properties\":["
                "{\"interface\":\"org.freedesktop.systemd1.Unit\",\"name\":\"FragmentPath\","
                "\"signature\":\"s\",\"value\":\"/etc/systemd/system/demo.service\"},"
                "{\"interface\":\"org.freedesktop.systemd1.Service\",\"name\":\"MainPID\","
                "\"signature\":\"u\",\"value\":4242},"
                "{\"interface\":\"org.freedesktop.systemd1.Service\",\"name\":\"ExecStart\","
                "\"signature\":\"a(sasbttttuii)\","
                "\"value\":[[\"/usr/bin/demo\",[\"/usr/bin/demo\",\"--serve\"],false,0,0,0,0,0,0,0]]}]}";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *response = NULL, *socket_path = NULL;
        Systemd1BrokerBackendUnitSnapshot *snapshot = NULL;
        const Systemd1BrokerBackendOps *ops;
        const Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = "demo@blue.service",
        };
        HTTPStub stub;

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "servicectl.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET", socket_path, true));
        ASSERT_OK(make_chunked_response(body, 109, &response));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_OK(ops->get_unit_snapshot(ops->userdata, extra.id, &extra, &snapshot));
        http_stub_done(&stub);

        ASSERT_NOT_NULL(snapshot);
        ASSERT_EQ(snapshot->size, sizeof(Systemd1BrokerBackendUnitSnapshot));
        ASSERT_EQ(snapshot->state, SYSTEMD1_BROKER_BACKEND_RUNNING);
        ASSERT_STREQ(snapshot->description, "Demo & Worker");
        ASSERT_EQ(snapshot->n_properties, 3u);
        ASSERT_EQ(snapshot->properties[0].size, sizeof(Systemd1BrokerBackendProperty));
        ASSERT_STREQ(snapshot->properties[0].interface, "org.freedesktop.systemd1.Unit");
        ASSERT_STREQ(snapshot->properties[0].name, "FragmentPath");
        ASSERT_STREQ(snapshot->properties[0].signature, "s");
        ASSERT_STREQ(snapshot->properties[0].value_json, "\"/etc/systemd/system/demo.service\"");
        ASSERT_STREQ(snapshot->properties[1].value_json, "4242");
        ASSERT_STREQ(snapshot->properties[2].value_json,
                     "[[\"/usr/bin/demo\",[\"/usr/bin/demo\",\"--serve\"],false,0,0,0,0,0,0,0]]");
        ASSERT_TRUE(startswith(stub.request, "GET /v1/units/demo%40blue.service HTTP/1.1\r\n"));

        ops->free_unit_snapshot(ops->userdata, snapshot);
        free(stub.request);
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET"));
}

TEST(get_unit_snapshot_maps_missing_unit_to_absent) {
        static const char response[] = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *socket_path = NULL;
        Systemd1BrokerBackendUnitSnapshot *snapshot = NULL;
        const Systemd1BrokerBackendOps *ops;
        const Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = "missing.service",
        };
        HTTPStub stub;

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "servicectl.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET", socket_path, true));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_OK(ops->get_unit_snapshot(ops->userdata, extra.id, &extra, &snapshot));
        http_stub_done(&stub);

        ASSERT_NOT_NULL(snapshot);
        ASSERT_EQ(snapshot->state, SYSTEMD1_BROKER_BACKEND_ABSENT);
        ASSERT_NULL(snapshot->description);
        ASSERT_NULL(snapshot->properties);
        ASSERT_EQ(snapshot->n_properties, 0u);
        ops->free_unit_snapshot(ops->userdata, snapshot);

        free(stub.request);
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET"));
}

TEST(get_unit_snapshot_rejects_malformed_property) {
        static const char body[] =
                "{\"unit\":{\"name\":\"demo.service\",\"description\":\"Demo\",\"state\":\"STARTED\"},"
                "\"systemd_properties\":["
                "{\"interface\":\"org.freedesktop.systemd1.Unit\",\"name\":\"Description\","
                "\"signature\":\"s\",\"value\":\"Demo\"},"
                "{\"interface\":\"org.freedesktop.systemd1.Unit\","
                "\"name\":\"FragmentPath\",\"value\":\"/etc/systemd/system/demo.service\"}]}";
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *response = NULL, *socket_path = NULL;
        Systemd1BrokerBackendUnitSnapshot *snapshot = NULL;
        const Systemd1BrokerBackendOps *ops;
        const Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = "demo.service",
        };
        HTTPStub stub;

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "servicectl.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET", socket_path, true));
        ASSERT_OK(asprintf(&response,
                           "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           strlen(body), body));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_ERROR(ops->get_unit_snapshot(ops->userdata, extra.id, &extra, &snapshot), EBADMSG);
        http_stub_done(&stub);
        ASSERT_NULL(snapshot);

        free(stub.request);
        ASSERT_OK_ERRNO(unsetenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET"));
}

TEST(get_unit_snapshot_rejects_property_limit) {
        _cleanup_(dlclosep) void *dl = NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmp = NULL;
        _cleanup_free_ char *body = NULL, *response = NULL, *socket_path = NULL;
        Systemd1BrokerBackendUnitSnapshot *snapshot = NULL;
        const Systemd1BrokerBackendOps *ops;
        const Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = "demo.service",
        };
        HTTPStub stub;
        FILE *stream;
        size_t body_size = 0;

        stream = ASSERT_PTR(open_memstream(&body, &body_size));
        ASSERT_GT(fprintf(stream,
                          "{\"unit\":{\"name\":\"demo.service\",\"description\":\"Demo\",\"state\":\"STARTED\"},"
                          "\"systemd_properties\":["), 0);
        for (unsigned i = 0; i < 257; i++)
                ASSERT_GT(fprintf(stream,
                                  "%s{\"interface\":\"org.freedesktop.systemd1.Unit\","
                                  "\"name\":\"Property%u\",\"signature\":\"u\",\"value\":%u}",
                                  i == 0 ? "" : ",", i, i), 0);
        ASSERT_GT(fprintf(stream, "]}"), 0);
        ASSERT_OK_ERRNO(fclose(stream));

        ASSERT_OK(mkdtemp_malloc(NULL, &tmp));
        socket_path = path_join(tmp, "servicectl.sock");
        ASSERT_NOT_NULL(socket_path);
        ASSERT_OK_ERRNO(setenv("SYSTEMD1_BROKER_SERVICECTL_SOCKET", socket_path, true));
        ASSERT_OK(asprintf(&response,
                           "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           body_size, body));
        ops = load_backend(&dl);

        http_stub_start(&stub, socket_path, response);
        ASSERT_ERROR(ops->get_unit_snapshot(ops->userdata, extra.id, &extra, &snapshot), E2BIG);
        http_stub_done(&stub);
        ASSERT_NULL(snapshot);
        ops->free_unit_snapshot(ops->userdata, NULL);

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

                ASSERT_OK(make_chunked_response(test->body, 11, &response));
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
