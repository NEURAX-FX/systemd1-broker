/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <pthread.h>
#include <sys/socket.h>

#include "sd-bus.h"

#include "alloc-util.h"
#include "bus-internal.h"
#include "fd-util.h"
#include "path-util.h"
#include "pidref.h"
#include "process-util.h"
#include "rm-rf.h"
#include "socket-util.h"
#include "stat-util.h"
#include "strv.h"
#include "systemd1-broker-backend-api.h"
#include "systemd1-broker-bus.h"
#include "systemd1-broker-dbus.h"
#include "systemd1-broker-metadata.h"
#include "systemd1-broker.h"
#include "tests.h"
#include "time-util.h"
#include "tmpfile-util.h"

typedef struct BrokerBusTestContext {
        int fds[2];
        Systemd1BrokerManager *manager;
} BrokerBusTestContext;

typedef struct JobRemovedSignal {
        uint32_t id;
        char *path;
        char *unit;
        char *result;
} JobRemovedSignal;

typedef struct UnitPropertiesChangedSignal {
        char *interface;
        char *active_state;
        char *sub_state;
        uint32_t job_id;
        char *job_path;
        char **invalidated;
} UnitPropertiesChangedSignal;

typedef struct JobNewSignal {
        uint32_t id;
        char *path;
        char *unit;
} JobNewSignal;

static JobNewSignal* job_new_signal_done(JobNewSignal *signal) {
        if (!signal)
                return NULL;

        signal->path = mfree(signal->path);
        signal->unit = mfree(signal->unit);

        return NULL;
}

static UnitPropertiesChangedSignal* unit_properties_changed_signal_done(UnitPropertiesChangedSignal *signal) {
        if (!signal)
                return NULL;

        signal->interface = mfree(signal->interface);
        signal->active_state = mfree(signal->active_state);
        signal->sub_state = mfree(signal->sub_state);
        signal->job_path = mfree(signal->job_path);
        signal->invalidated = strv_free(signal->invalidated);

        return NULL;
}

static JobRemovedSignal* job_removed_signal_done(JobRemovedSignal *signal) {
        if (!signal)
                return NULL;

        signal->path = mfree(signal->path);
        signal->unit = mfree(signal->unit);
        signal->result = mfree(signal->result);

        return NULL;
}

typedef struct TestBackendContext {
        unsigned status_calls;
        unsigned start_calls;
        unsigned stop_calls;
        unsigned list_calls;
        unsigned snapshot_calls;
        unsigned snapshot_free_calls;
        const char *last_unit_name;
        Systemd1BrokerBackendState status_state;
        const Systemd1BrokerBackendUnit *units;
        size_t n_units;
        int list_error;
        const Systemd1BrokerBackendUnitSnapshot *snapshot;
        int snapshot_error;
} TestBackendContext;

static void test_backend_free_units(void *userdata, Systemd1BrokerBackendUnit *units, size_t n_units) {
        if (!units)
                return;

        for (size_t i = 0; i < n_units; i++) {
                free((char*) units[i].id);
                free((char*) units[i].description);
        }
        free(units);
}

static int test_backend_list_units(void *userdata, Systemd1BrokerBackendUnit **ret_units, size_t *ret_n_units) {
        TestBackendContext *context = ASSERT_PTR(userdata);
        Systemd1BrokerBackendUnit *units = NULL;

        context->list_calls++;
        if (context->list_error < 0)
                return context->list_error;

        if (context->n_units > 0) {
                units = new0(Systemd1BrokerBackendUnit, context->n_units);
                ASSERT_NOT_NULL(units);
        }
        for (size_t i = 0; i < context->n_units; i++) {
                units[i] = context->units[i];
                units[i].id = ASSERT_PTR(strdup(context->units[i].id));
                units[i].description = ASSERT_PTR(strdup(context->units[i].description ?: ""));
        }

        *ret_units = units;
        *ret_n_units = context->n_units;
        return 0;
}

static void test_backend_free_unit_snapshot(void *userdata, Systemd1BrokerBackendUnitSnapshot *snapshot) {
        TestBackendContext *context = ASSERT_PTR(userdata);
        Systemd1BrokerBackendProperty *properties;

        if (!snapshot)
                return;

        context->snapshot_free_calls++;

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

static int test_backend_get_unit_snapshot(
                void *userdata,
                const char *unit_name,
                const Systemd1BrokerBackendUnitExtra *extra,
                Systemd1BrokerBackendUnitSnapshot **ret_snapshot) {

        TestBackendContext *context = ASSERT_PTR(userdata);
        const Systemd1BrokerBackendUnitSnapshot *source = context->snapshot;
        Systemd1BrokerBackendUnitSnapshot *snapshot;
        Systemd1BrokerBackendProperty *properties = NULL;

        context->snapshot_calls++;
        context->last_unit_name = unit_name;
        ASSERT_NOT_NULL(extra);
        if (context->snapshot_error < 0)
                return context->snapshot_error;

        snapshot = new0(Systemd1BrokerBackendUnitSnapshot, 1);
        ASSERT_NOT_NULL(snapshot);
        snapshot->size = sizeof(Systemd1BrokerBackendUnitSnapshot);
        snapshot->state = source ? source->state : context->status_state;
        if (source && source->description)
                snapshot->description = ASSERT_PTR(strdup(source->description));
        if (source && source->n_properties > 0) {
                properties = new0(Systemd1BrokerBackendProperty, source->n_properties);
                ASSERT_NOT_NULL(properties);
                for (size_t i = 0; i < source->n_properties; i++) {
                        properties[i].size = sizeof(Systemd1BrokerBackendProperty);
                        properties[i].interface = ASSERT_PTR(strdup(source->properties[i].interface));
                        properties[i].name = ASSERT_PTR(strdup(source->properties[i].name));
                        properties[i].signature = ASSERT_PTR(strdup(source->properties[i].signature));
                        properties[i].value_json = ASSERT_PTR(strdup(source->properties[i].value_json));
                }
                snapshot->properties = properties;
                snapshot->n_properties = source->n_properties;
        }

        *ret_snapshot = snapshot;
        return 0;
}

static int test_backend_status(
                void *userdata,
                const char *unit_name,
                const Systemd1BrokerBackendUnitExtra *extra,
                Systemd1BrokerBackendState *ret_state) {

        TestBackendContext *context = ASSERT_PTR(userdata);

        ASSERT_NOT_NULL(unit_name);
        ASSERT_NOT_NULL(extra);
        ASSERT_EQ(extra->size, sizeof(Systemd1BrokerBackendUnitExtra));
        ASSERT_NOT_NULL(ret_state);

        context->status_calls++;
        context->last_unit_name = unit_name;
        *ret_state = context->status_state;
        return 0;
}

static int test_backend_start(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra) {
        TestBackendContext *context = ASSERT_PTR(userdata);

        ASSERT_NOT_NULL(unit_name);
        ASSERT_NOT_NULL(extra);
        ASSERT_EQ(extra->size, sizeof(Systemd1BrokerBackendUnitExtra));

        context->start_calls++;
        context->last_unit_name = unit_name;
        return 0;
}

static int test_backend_stop(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra) {
        TestBackendContext *context = ASSERT_PTR(userdata);

        ASSERT_NOT_NULL(unit_name);
        ASSERT_NOT_NULL(extra);
        ASSERT_EQ(extra->size, sizeof(Systemd1BrokerBackendUnitExtra));

        context->stop_calls++;
        context->last_unit_name = unit_name;
        return 0;
}

#define TEST_BACKEND_OPS(context)                                     \
        {                                                             \
                .size = sizeof(Systemd1BrokerBackendOps),             \
                .userdata = (context),                                \
                .status = test_backend_status,                        \
                .start = test_backend_start,                          \
                .stop = test_backend_stop,                            \
                .list_units = test_backend_list_units,                \
                .free_units = test_backend_free_units,                \
                .get_unit_snapshot = test_backend_get_unit_snapshot,  \
                .free_unit_snapshot = test_backend_free_unit_snapshot, \
        }

static int match_job_new(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        JobNewSignal *signal = ASSERT_PTR(userdata);
        const char *path, *unit;
        int r;

        r = sd_bus_message_read(message, "uos", &signal->id, &path, &unit);
        if (r < 0)
                return r;

        ASSERT_OK(free_and_strdup(&signal->path, path));
        ASSERT_OK(free_and_strdup(&signal->unit, unit));

        return 0;
}

static int match_job_removed(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        JobRemovedSignal *signal = ASSERT_PTR(userdata);
        const char *path, *unit, *result;
        int r;

        r = sd_bus_message_read(message, "uoss", &signal->id, &path, &unit, &result);
        if (r < 0)
                return r;

        ASSERT_OK(free_and_strdup(&signal->path, path));
        ASSERT_OK(free_and_strdup(&signal->unit, unit));
        ASSERT_OK(free_and_strdup(&signal->result, result));

        return 0;
}

static int match_unit_properties_changed(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        UnitPropertiesChangedSignal *signal = ASSERT_PTR(userdata);
        const char *interface, *property;
        int r;

        r = sd_bus_message_read(message, "s", &interface);
        if (r < 0)
                return r;

        ASSERT_OK(free_and_strdup(&signal->interface, interface));
        ASSERT_OK(sd_bus_message_enter_container(message, 'a', "{sv}"));
        while (ASSERT_OK(sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
                ASSERT_OK(sd_bus_message_read_basic(message, 's', &property));

                if (streq(property, "ActiveState")) {
                        const char *active_state;

                        ASSERT_OK(sd_bus_message_enter_container(message, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(message, "s", &active_state));
                        ASSERT_OK(free_and_strdup(&signal->active_state, active_state));
                        ASSERT_OK(sd_bus_message_exit_container(message));
                } else if (streq(property, "SubState")) {
                        const char *sub_state;

                        ASSERT_OK(sd_bus_message_enter_container(message, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(message, "s", &sub_state));
                        ASSERT_OK(free_and_strdup(&signal->sub_state, sub_state));
                        ASSERT_OK(sd_bus_message_exit_container(message));
                } else if (streq(property, "Job")) {
                        const char *job_path;

                        ASSERT_OK(sd_bus_message_enter_container(message, 'v', "(uo)"));
                        ASSERT_OK(sd_bus_message_read(message, "(uo)", &signal->job_id, &job_path));
                        ASSERT_OK(free_and_strdup(&signal->job_path, job_path));
                        ASSERT_OK(sd_bus_message_exit_container(message));
                } else
                        ASSERT_OK(sd_bus_message_skip(message, "v"));

                ASSERT_OK(sd_bus_message_exit_container(message));
        }
        ASSERT_OK(sd_bus_message_exit_container(message));
        signal->invalidated = strv_free(signal->invalidated);
        ASSERT_OK(sd_bus_message_read_strv(message, &signal->invalidated));

        return 0;
}

static int new_method_return_message(sd_bus_message **ret) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;

        ASSERT_OK(sd_bus_new(&bus));
        bus->state = BUS_RUNNING;
        return sd_bus_message_new(bus, ret, SD_BUS_MESSAGE_METHOD_RETURN);
}

static int new_property_value_message(const Systemd1BrokerProperty *property, sd_bus_message **ret) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *message = NULL;
        int r;

        r = new_method_return_message(&message);
        if (r < 0)
                return r;
        r = systemd1_broker_property_append_value(message, property);
        if (r < 0)
                return r;
        r = sd_bus_message_seal(message, 1, 0);
        if (r < 0)
                return r;
        r = sd_bus_message_rewind(message, true);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(message);
        return 0;
}

static void* broker_bus_test_server(void *userdata) {
        BrokerBusTestContext *context = ASSERT_PTR(userdata);
        bool quit = false;
        int fd;

        fd = TAKE_FD(context->fds[0]);
        ASSERT_OK(systemd1_broker_serve_bus_fd_full(fd, context->manager, true, &quit));

        return NULL;
}

static int call_manager_method_strv(
                sd_bus *bus,
                const char *method,
                char **arg1,
                char **arg2,
                sd_bus_error *error,
                sd_bus_message **reply) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        int r;

        assert(bus);
        assert(method);
        assert(reply);

        r = sd_bus_message_new_method_call(
                        bus,
                        &m,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        method);
        if (r < 0)
                return r;

        r = sd_bus_message_append_strv(m, arg1);
        if (r < 0)
                return r;

        if (arg2) {
                r = sd_bus_message_append_strv(m, arg2);
                if (r < 0)
                        return r;
        }

        return sd_bus_call(bus, m, 0, error, reply);
}

static int broker_bus_test_client(BrokerBusTestContext *context) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_slot_unrefp) sd_bus_slot *job_new_slot = NULL;
        _cleanup_(sd_bus_slot_unrefp) sd_bus_slot *job_removed_slot = NULL;
        _cleanup_(sd_bus_slot_unrefp) sd_bus_slot *unit_properties_slot = NULL;
        _cleanup_(job_new_signal_done) JobNewSignal job_new = {};
        _cleanup_(job_removed_signal_done) JobRemovedSignal job_removed = {};
        _cleanup_(unit_properties_changed_signal_done) UnitPropertiesChangedSignal unit_properties_changed = {};
        const char *path, *id, *description, *load_state, *active_state, *sub_state, *following, *job_type, *job_path, *unit_id, *job_state, *unit_path, *property;
        const char *version = NULL, *features = NULL, *virtualization = NULL, *architecture = NULL, *control_group = NULL, *system_state = NULL;
        const char *introspection;
        char **environment = NULL;
        bool found_id = false, found_description = false, found_active_state = false, found_need_daemon_reload = false, found_job = false;
        bool found_version = false, found_features = false, found_virtualization = false, found_architecture = false;
        bool found_n_names = false, found_n_jobs = false, found_environment = false, found_control_group = false, found_system_state = false;
        bool found_service_main_pid = false, found_service_result = false, found_service_status_errno = false;
        bool found_service_start_timestamp = false, found_service_pid_file = false;
        uint32_t job_id, n_names, n_jobs;
        int fd;

        ASSERT_OK(sd_bus_new(&bus));
        fd = TAKE_FD(context->fds[1]);
        ASSERT_OK(sd_bus_set_fd(bus, fd, fd));
        ASSERT_OK(sd_bus_start(bus));
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "Subscribe", &error, NULL, NULL));
        ASSERT_OK(sd_bus_match_signal(
                        bus,
                        &job_new_slot,
                        NULL,
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "JobNew",
                        match_job_new,
                        &job_new));
        ASSERT_OK(sd_bus_match_signal(
                        bus,
                        &job_removed_slot,
                        NULL,
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "JobRemoved",
                        match_job_removed,
                        &job_removed));
        ASSERT_OK(sd_bus_match_signal(
                        bus,
                        &unit_properties_slot,
                        NULL,
                        "/org/freedesktop/systemd1/unit/alpha_2eservice",
                        "org.freedesktop.DBus.Properties",
                        "PropertiesChanged",
                        match_unit_properties_changed,
                        &unit_properties_changed));

        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "GetUnit", &error, &reply, "s", "alpha.service"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/alpha_2eservice");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.DBus.Introspectable", "Introspect", &error, &reply, NULL));
        ASSERT_OK(sd_bus_message_read(reply, "s", &introspection));
        ASSERT_TRUE(strstr(introspection, "interface name=\"org.freedesktop.systemd1.Manager\""));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "LoadUnit", &error, &reply, "s", "alpha.service"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/alpha_2eservice");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "ListUnits", &error, &reply, NULL));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "(ssssssouso)"));
        ASSERT_OK(sd_bus_message_read(reply, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_STREQ(id, "alpha.service");
        ASSERT_STREQ(description, "Alpha");
        ASSERT_STREQ(load_state, "loaded");
        ASSERT_STREQ(active_state, "inactive");
        ASSERT_STREQ(sub_state, "dead");
        ASSERT_STREQ(following, "");
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/alpha_2eservice");
        ASSERT_EQ(job_id, 0u);
        ASSERT_STREQ(job_type, "");
        ASSERT_STREQ(job_path, "/");
        ASSERT_OK_ZERO(sd_bus_message_read(reply, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "ListJobs", &error, &reply, NULL));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "(usssoo)"));
        ASSERT_OK_ZERO(sd_bus_message_read(reply, "(usssoo)", &job_id, &unit_id, &job_type, &job_state, &job_path, &unit_path));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "TryRestartUnit",
                        &error,
                        &reply,
                        "ss",
                        "alpha.service",
                        "replace"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &job_path));
        ASSERT_STREQ(job_path, "/");
        ASSERT_EQ(systemd1_broker_manager_n_jobs(context->manager), 0u);
        ASSERT_NULL(job_new.path);

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "StartUnit", &error, &reply, "ss", "alpha.service", "replace"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &job_path));
        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/1");
        for (unsigned i = 0; i < 20 && (!job_new.path || !unit_properties_changed.job_path); i++)
                ASSERT_OK(sd_bus_process(bus, NULL));
        ASSERT_NOT_NULL(job_new.path);
        ASSERT_EQ(job_new.id, 1u);
        ASSERT_STREQ(job_new.path, "/org/freedesktop/systemd1/job/1");
        ASSERT_STREQ(job_new.unit, "alpha.service");
        ASSERT_STREQ(unit_properties_changed.interface, "org.freedesktop.systemd1.Unit");
        ASSERT_EQ(unit_properties_changed.job_id, 1u);
        ASSERT_STREQ(unit_properties_changed.job_path, "/org/freedesktop/systemd1/job/1");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "ListJobs", &error, &reply, NULL));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "(usssoo)"));
        ASSERT_OK(sd_bus_message_read(reply, "(usssoo)", &job_id, &unit_id, &job_type, &job_state, &job_path, &unit_path));
        ASSERT_EQ(job_id, 1u);
        ASSERT_STREQ(unit_id, "alpha.service");
        ASSERT_STREQ(job_type, "start");
        ASSERT_STREQ(job_state, "waiting");
        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/1");
        ASSERT_STREQ(unit_path, "/org/freedesktop/systemd1/unit/alpha_2eservice");
        ASSERT_OK_ZERO(sd_bus_message_read(reply, "(usssoo)", &job_id, &unit_id, &job_type, &job_state, &job_path, &unit_path));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.DBus.Properties", "GetAll", &error, &reply, "s", "org.freedesktop.systemd1.Manager"));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "{sv}"));
        while (ASSERT_OK(sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
                ASSERT_OK(sd_bus_message_read_basic(reply, 's', &property));

                if (streq(property, "Version")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &version));
                        ASSERT_FALSE(isempty(version));
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_version = true;
                } else if (streq(property, "Features")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &features));
                        ASSERT_NOT_NULL(features);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_features = true;
                } else if (streq(property, "Virtualization")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &virtualization));
                        ASSERT_STREQ(virtualization, "");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_virtualization = true;
                } else if (streq(property, "Architecture")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &architecture));
                        ASSERT_FALSE(isempty(architecture));
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_architecture = true;
                } else if (streq(property, "NNames")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "u"));
                        ASSERT_OK(sd_bus_message_read(reply, "u", &n_names));
                        ASSERT_EQ(n_names, 1u);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_n_names = true;
                } else if (streq(property, "NJobs")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "u"));
                        ASSERT_OK(sd_bus_message_read(reply, "u", &n_jobs));
                        ASSERT_EQ(n_jobs, 1u);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_n_jobs = true;
                } else if (streq(property, "Environment")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "as"));
                        ASSERT_OK(sd_bus_message_read_strv(reply, &environment));
                        ASSERT_TRUE(strv_isempty(environment));
                        environment = strv_free(environment);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_environment = true;
                } else if (streq(property, "ControlGroup")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &control_group));
                        ASSERT_STREQ(control_group, "");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_control_group = true;
                } else if (streq(property, "SystemState")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &system_state));
                        ASSERT_STREQ(system_state, "running");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_system_state = true;
                } else
                        ASSERT_OK(sd_bus_message_skip(reply, "v"));

                ASSERT_OK(sd_bus_message_exit_container(reply));
        }
        ASSERT_OK(sd_bus_message_exit_container(reply));
        ASSERT_TRUE(found_version);
        ASSERT_TRUE(found_features);
        ASSERT_TRUE(found_virtualization);
        ASSERT_TRUE(found_architecture);
        ASSERT_TRUE(found_n_names);
        ASSERT_TRUE(found_n_jobs);
        ASSERT_TRUE(found_environment);
        ASSERT_TRUE(found_control_group);
        ASSERT_TRUE(found_system_state);

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1/job/1", "org.freedesktop.DBus.Properties", "GetAll", &error, &reply, "s", "org.freedesktop.systemd1.Job"));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "{sv}"));
        while (ASSERT_OK(sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
                ASSERT_OK(sd_bus_message_read_basic(reply, 's', &property));

                if (streq(property, "Id")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "u"));
                        ASSERT_OK(sd_bus_message_read(reply, "u", &job_id));
                        ASSERT_EQ(job_id, 1u);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                } else if (streq(property, "JobType")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &job_type));
                        ASSERT_STREQ(job_type, "start");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                } else if (streq(property, "State")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &job_state));
                        ASSERT_STREQ(job_state, "waiting");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                } else if (streq(property, "Unit")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "(so)"));
                        ASSERT_OK(sd_bus_message_read(reply, "(so)", &unit_id, &unit_path));
                        ASSERT_STREQ(unit_id, "alpha.service");
                        ASSERT_STREQ(unit_path, "/org/freedesktop/systemd1/unit/alpha_2eservice");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                } else
                        ASSERT_OK(sd_bus_message_skip(reply, "v"));

                ASSERT_OK(sd_bus_message_exit_container(reply));
        }
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1/job/1", "org.freedesktop.DBus.Introspectable", "Introspect", &error, &reply, NULL));
        ASSERT_OK(sd_bus_message_read(reply, "s", &introspection));
        ASSERT_TRUE(strstr(introspection, "interface name=\"org.freedesktop.systemd1.Job\""));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "GetJob", &error, &reply, "u", UINT32_C(1)));
        ASSERT_OK(sd_bus_message_read(reply, "o", &job_path));
        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/1");

        reply = sd_bus_message_unref(reply);
        ASSERT_ERROR(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "CancelJob", &error, &reply, "u", UINT32_C(1)), EOPNOTSUPP);
        sd_bus_error_free(&error);

        unit_properties_changed_signal_done(&unit_properties_changed);
        unit_properties_changed.job_id = UINT32_MAX;

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/broker/test", "org.freedesktop.systemd1.BrokerTest", "CompleteJob", &error, NULL, "u", UINT32_C(1)));
        for (unsigned i = 0; i < 20 && (!job_removed.path || !unit_properties_changed.active_state); i++)
                ASSERT_OK(sd_bus_process(bus, NULL));
        ASSERT_NOT_NULL(unit_properties_changed.active_state);
        ASSERT_STREQ(unit_properties_changed.interface, "org.freedesktop.systemd1.Unit");
        ASSERT_STREQ(unit_properties_changed.active_state, "active");
        ASSERT_STREQ(unit_properties_changed.sub_state, "running");
        ASSERT_EQ(unit_properties_changed.job_id, 0u);
        ASSERT_STREQ(unit_properties_changed.job_path, "/");
        ASSERT_NOT_NULL(job_removed.path);
        ASSERT_EQ(job_removed.id, 1u);
        ASSERT_STREQ(job_removed.path, "/org/freedesktop/systemd1/job/1");
        ASSERT_STREQ(job_removed.unit, "alpha.service");
        ASSERT_STREQ(job_removed.result, "done");

        job_new_signal_done(&job_new);
        job_new.id = 0;
        unit_properties_changed_signal_done(&unit_properties_changed);
        unit_properties_changed.job_id = UINT32_MAX;

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1/unit/alpha_2eservice", "org.freedesktop.DBus.Introspectable", "Introspect", &error, &reply, NULL));
        ASSERT_OK(sd_bus_message_read(reply, "s", &introspection));
        ASSERT_TRUE(strstr(introspection, "interface name=\"org.freedesktop.systemd1.Unit\""));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1/unit/alpha_2eservice", "org.freedesktop.systemd1.Unit", "Restart", &error, &reply, "s", "replace"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &job_path));
        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/2");
        for (unsigned i = 0; i < 20 && (!job_new.path || !unit_properties_changed.job_path); i++)
                ASSERT_OK(sd_bus_process(bus, NULL));
        ASSERT_NOT_NULL(job_new.path);
        ASSERT_EQ(job_new.id, 2u);
        ASSERT_STREQ(job_new.path, "/org/freedesktop/systemd1/job/2");
        ASSERT_STREQ(job_new.unit, "alpha.service");
        ASSERT_EQ(unit_properties_changed.job_id, 2u);
        ASSERT_STREQ(unit_properties_changed.job_path, "/org/freedesktop/systemd1/job/2");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "Reload", &error, NULL, NULL));
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "ResetFailed", &error, NULL, NULL));
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "ResetFailedUnit", &error, NULL, "s", "alpha.service"));

        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "Unsubscribe", &error, NULL, NULL));

        ASSERT_OK(systemd1_broker_manager_complete_job(context->manager, 2));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(call_manager_method_strv(
                        bus,
                        "ListUnitsFiltered",
                        STRV_MAKE("active"),
                        NULL,
                        &error,
                        &reply));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "(ssssssouso)"));
        ASSERT_OK(sd_bus_message_read(reply, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_STREQ(id, "alpha.service");
        ASSERT_STREQ(active_state, "active");
        ASSERT_OK_ZERO(sd_bus_message_read(reply, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(call_manager_method_strv(
                        bus,
                        "ListUnitsByPatterns",
                        STRV_MAKE("running"),
                        STRV_MAKE("alpha.*"),
                        &error,
                        &reply));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "(ssssssouso)"));
        ASSERT_OK(sd_bus_message_read(reply, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_STREQ(id, "alpha.service");
        ASSERT_STREQ(sub_state, "running");
        ASSERT_OK_ZERO(sd_bus_message_read(reply, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(call_manager_method_strv(
                        bus,
                        "ListUnitsByNames",
                        STRV_MAKE("alpha.service"),
                        NULL,
                        &error,
                        &reply));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "(ssssssouso)"));
        ASSERT_OK(sd_bus_message_read(reply, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_STREQ(id, "alpha.service");
        ASSERT_OK_ZERO(sd_bus_message_read(reply, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "ReloadUnit",
                        &error,
                        &reply,
                        "ss",
                        "alpha.service",
                        "replace"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &job_path));
        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/3");

        ASSERT_OK(systemd1_broker_manager_complete_job(context->manager, 3));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "TryRestartUnit",
                        &error,
                        &reply,
                        "ss",
                        "alpha.service",
                        "replace"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &job_path));
        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/4");

        ASSERT_OK(systemd1_broker_manager_complete_job(context->manager, 4));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "ReloadOrRestartUnit",
                        &error,
                        &reply,
                        "ss",
                        "alpha.service",
                        "replace"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &job_path));
        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/5");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1/unit/alpha_2eservice", "org.freedesktop.DBus.Properties", "GetAll", &error, &reply, "s", "org.freedesktop.systemd1.Unit"));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "{sv}"));
        while (ASSERT_OK(sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
                ASSERT_OK(sd_bus_message_read_basic(reply, 's', &property));

                if (streq(property, "Id")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &id));
                        ASSERT_STREQ(id, "alpha.service");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_id = true;
                } else if (streq(property, "Description")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &description));
                        ASSERT_STREQ(description, "Alpha");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_description = true;
                } else if (streq(property, "ActiveState")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &active_state));
                        ASSERT_STREQ(active_state, "active");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_active_state = true;
                } else if (streq(property, "NeedDaemonReload")) {
                        int need_daemon_reload;

                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "b"));
                        ASSERT_OK(sd_bus_message_read(reply, "b", &need_daemon_reload));
                        ASSERT_FALSE(need_daemon_reload);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_need_daemon_reload = true;
                } else if (streq(property, "Job")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "(uo)"));
                        ASSERT_OK(sd_bus_message_read(reply, "(uo)", &job_id, &job_path));
                        ASSERT_EQ(job_id, 5u);
                        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/5");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_job = true;
                } else
                        ASSERT_OK(sd_bus_message_skip(reply, "v"));

                ASSERT_OK(sd_bus_message_exit_container(reply));
        }
        ASSERT_OK(sd_bus_message_exit_container(reply));
        ASSERT_TRUE(found_id);
        ASSERT_TRUE(found_description);
        ASSERT_TRUE(found_active_state);
        ASSERT_FALSE(found_need_daemon_reload);
        ASSERT_TRUE(found_job);

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1/unit/alpha_2eservice",
                        "org.freedesktop.DBus.Introspectable",
                        "Introspect",
                        &error,
                        &reply,
                        NULL));
        ASSERT_OK(sd_bus_message_read(reply, "s", &introspection));
        ASSERT_TRUE(strstr(introspection, "interface name=\"org.freedesktop.systemd1.Service\""));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1/unit/alpha_2eservice",
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        &error,
                        &reply,
                        "s",
                        "org.freedesktop.systemd1.Service"));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "{sv}"));
        while (ASSERT_OK(sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
                ASSERT_OK(sd_bus_message_read_basic(reply, 's', &property));

                if (streq(property, "MainPID")) {
                        uint32_t main_pid;

                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "u"));
                        ASSERT_OK(sd_bus_message_read(reply, "u", &main_pid));
                        ASSERT_EQ(main_pid, 0u);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_service_main_pid = true;
                } else if (streq(property, "Result")) {
                        const char *result;

                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &result));
                        ASSERT_STREQ(result, "success");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_service_result = true;
                } else if (streq(property, "StatusErrno")) {
                        int32_t status_errno;

                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "i"));
                        ASSERT_OK(sd_bus_message_read(reply, "i", &status_errno));
                        ASSERT_EQ(status_errno, 0);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_service_status_errno = true;
                } else if (streq(property, "ExecMainStartTimestamp")) {
                        uint64_t start_timestamp;

                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "t"));
                        ASSERT_OK(sd_bus_message_read(reply, "t", &start_timestamp));
                        ASSERT_EQ(start_timestamp, UINT64_C(0));
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_service_start_timestamp = true;
                } else if (streq(property, "PIDFile")) {
                        const char *pid_file;

                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &pid_file));
                        ASSERT_STREQ(pid_file, "");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_service_pid_file = true;
                } else
                        ASSERT_OK(sd_bus_message_skip(reply, "v"));

                ASSERT_OK(sd_bus_message_exit_container(reply));
        }
        ASSERT_OK(sd_bus_message_exit_container(reply));
        ASSERT_FALSE(found_service_main_pid);
        ASSERT_FALSE(found_service_result);
        ASSERT_FALSE(found_service_status_errno);
        ASSERT_FALSE(found_service_start_timestamp);
        ASSERT_FALSE(found_service_pid_file);

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "LoadUnit", &error, &reply, "s", "systemd-timesyncd.service"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/systemd_2dtimesyncd_2eservice");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1/unit/systemd_2dtimesyncd_2eservice", "org.freedesktop.DBus.Properties", "GetAll", &error, &reply, "s", "org.freedesktop.systemd1.Unit"));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "{sv}"));
        found_id = false;
        while (ASSERT_OK(sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
                ASSERT_OK(sd_bus_message_read_basic(reply, 's', &property));

                if (streq(property, "Id")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &id));
                        ASSERT_STREQ(id, "systemd-timesyncd.service");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_id = true;
                } else
                        ASSERT_OK(sd_bus_message_skip(reply, "v"));

                ASSERT_OK(sd_bus_message_exit_container(reply));
        }
        ASSERT_OK(sd_bus_message_exit_container(reply));
        ASSERT_TRUE(found_id);

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/broker/test", "org.freedesktop.systemd1.BrokerTest", "Exit", &error, NULL, NULL));

        return 0;
}

TEST(dbus_manager_get_unit_and_list_units) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        BrokerBusTestContext context = {
                .fds = EBADF_PAIR,
        };
        pthread_t server;
        void *server_result;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));

        ASSERT_OK_ERRNO(socketpair(AF_UNIX, SOCK_STREAM, 0, context.fds));
        context.manager = manager;

        ASSERT_OK_ERRNO(pthread_create(&server, NULL, broker_bus_test_server, &context));
        ASSERT_OK(broker_bus_test_client(&context));
        ASSERT_OK_ERRNO(pthread_join(server, &server_result));
        ASSERT_NULL(server_result);
}

TEST(dbus_dynamic_unit_properties) {
        static const Systemd1BrokerBackendProperty properties[] = {
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "FragmentPath",
                        .signature = "s",
                        .value_json = "\"/etc/systemd/system/demo.service\"",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "MainPID",
                        .signature = "u",
                        .value_json = "4242",
                },
        };
        static const Systemd1BrokerBackendUnitSnapshot snapshot = {
                .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                .description = "Demo Updated",
                .properties = properties,
                .n_properties = ELEMENTSOF(properties),
        };
        static const Systemd1BrokerBackendProperty changed_properties[] = {
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "UnitFileState",
                        .signature = "s",
                        .value_json = "\"enabled\"",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "MainPID",
                        .signature = "u",
                        .value_json = "4242",
                },
        };
        static const Systemd1BrokerBackendUnitSnapshot changed_snapshot = {
                .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                .description = "Demo Updated",
                .properties = changed_properties,
                .n_properties = ELEMENTSOF(changed_properties),
        };
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_slot_unrefp) sd_bus_slot *properties_slot = NULL;
        _cleanup_(unit_properties_changed_signal_done) UnitPropertiesChangedSignal properties_changed = {};
        TestBackendContext backend_context = { .snapshot = &snapshot };
        const Systemd1BrokerBackendOps backend_ops = TEST_BACKEND_OPS(&backend_context);
        BrokerBusTestContext context = { .fds = EBADF_PAIR };
        bool found_id = false, found_description = false, found_fragment = false, found_main_pid = false;
        bool found_placeholder = false;
        const char *property, *string, *introspection;
        pthread_t server;
        void *server_result;
        uint32_t main_pid;
        int fd, r;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &backend_ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "demo.service", "Demo", NULL));
        ASSERT_OK_ERRNO(socketpair(AF_UNIX, SOCK_STREAM, 0, context.fds));
        context.manager = manager;
        ASSERT_OK_ERRNO(pthread_create(&server, NULL, broker_bus_test_server, &context));

        ASSERT_OK(sd_bus_new(&bus));
        fd = TAKE_FD(context.fds[1]);
        ASSERT_OK(sd_bus_set_fd(bus, fd, fd));
        ASSERT_OK(sd_bus_start(bus));
        ASSERT_OK(sd_bus_match_signal(
                        bus,
                        &properties_slot,
                        NULL,
                        "/org/freedesktop/systemd1/unit/demo_2eservice",
                        "org.freedesktop.DBus.Properties",
                        "PropertiesChanged",
                        match_unit_properties_changed,
                        &properties_changed));

        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1/unit/demo_2eservice",
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        &error,
                        &reply,
                        "s",
                        ""));
        ASSERT_EQ(backend_context.snapshot_calls, 1u);
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "{sv}"));
        while (ASSERT_OK(sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
                ASSERT_OK(sd_bus_message_read_basic(reply, 's', &property));
                if (streq(property, "Id")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &string));
                        ASSERT_STREQ(string, "demo.service");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_id = true;
                } else if (streq(property, "Description")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &string));
                        ASSERT_STREQ(string, "Demo Updated");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_description = true;
                } else if (streq(property, "FragmentPath")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "s"));
                        ASSERT_OK(sd_bus_message_read(reply, "s", &string));
                        ASSERT_STREQ(string, "/etc/systemd/system/demo.service");
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_fragment = true;
                } else if (streq(property, "MainPID")) {
                        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "u"));
                        ASSERT_OK(sd_bus_message_read(reply, "u", &main_pid));
                        ASSERT_EQ(main_pid, 4242u);
                        ASSERT_OK(sd_bus_message_exit_container(reply));
                        found_main_pid = true;
                } else {
                        found_placeholder |= STR_IN_SET(property, "Following", "NeedDaemonReload", "InvocationID", "Result");
                        ASSERT_OK(sd_bus_message_skip(reply, "v"));
                }
                ASSERT_OK(sd_bus_message_exit_container(reply));
        }
        ASSERT_OK(sd_bus_message_exit_container(reply));
        ASSERT_TRUE(found_id);
        ASSERT_TRUE(found_description);
        ASSERT_TRUE(found_fragment);
        ASSERT_TRUE(found_main_pid);
        ASSERT_FALSE(found_placeholder);

        while (ASSERT_OK(sd_bus_process(bus, NULL)) > 0)
                ;
        unit_properties_changed_signal_done(&properties_changed);

        backend_context.snapshot = &changed_snapshot;
        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1/unit/demo_2eservice",
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        &error,
                        &reply,
                        "s",
                        "org.freedesktop.systemd1.Unit"));
        for (unsigned i = 0; i < 20 && !properties_changed.invalidated; i++)
                ASSERT_OK(sd_bus_process(bus, NULL));
        ASSERT_STREQ(properties_changed.interface, "org.freedesktop.systemd1.Unit");
        ASSERT_TRUE(strv_contains(properties_changed.invalidated, "FragmentPath"));
        ASSERT_TRUE(strv_contains(properties_changed.invalidated, "UnitFileState"));
        unit_properties_changed_signal_done(&properties_changed);

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1/unit/demo_2eservice",
                        "org.freedesktop.DBus.Properties",
                        "Get",
                        &error,
                        &reply,
                        "ss",
                        "org.freedesktop.systemd1.Unit",
                        "DropInPaths"));
        ASSERT_EQ(backend_context.snapshot_calls, 3u);
        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "as"));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "s"));
        ASSERT_OK_ZERO(sd_bus_message_read_basic(reply, 's', &string));
        ASSERT_OK(sd_bus_message_exit_container(reply));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        r = sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1/unit/demo_2eservice",
                        "org.freedesktop.DBus.Properties",
                        "Get",
                        &error,
                        &reply,
                        "ss",
                        "org.freedesktop.systemd1.Unit",
                        "NeedDaemonReload");
        ASSERT_LT(r, 0);
        ASSERT_STREQ(error.name, SD_BUS_ERROR_UNKNOWN_PROPERTY);
        ASSERT_EQ(backend_context.snapshot_calls, 4u);
        sd_bus_error_free(&error);

        backend_context.snapshot = &snapshot;
        backend_context.snapshot_error = -EIO;
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1/unit/demo_2eservice",
                        "org.freedesktop.DBus.Properties",
                        "Get",
                        &error,
                        &reply,
                        "ss",
                        "org.freedesktop.systemd1.Service",
                        "MainPID"));
        ASSERT_EQ(backend_context.snapshot_calls, 5u);
        ASSERT_OK(sd_bus_message_enter_container(reply, 'v', "u"));
        ASSERT_OK(sd_bus_message_read(reply, "u", &main_pid));
        ASSERT_EQ(main_pid, 4242u);
        ASSERT_OK(sd_bus_message_exit_container(reply));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(
                        bus,
                        "org.freedesktop.systemd1",
                        "/org/freedesktop/systemd1/unit/demo_2eservice",
                        "org.freedesktop.DBus.Introspectable",
                        "Introspect",
                        &error,
                        &reply,
                        NULL));
        ASSERT_EQ(backend_context.snapshot_calls, 6u);
        ASSERT_OK(sd_bus_message_read(reply, "s", &introspection));
        ASSERT_TRUE(strstr(introspection, "property name=\"UnitFileState\" type=\"s\""));
        ASSERT_TRUE(strstr(introspection, "property name=\"MainPID\" type=\"u\""));
        ASSERT_FALSE(strstr(introspection, "property name=\"FragmentPath\""));

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/broker/test", "org.freedesktop.systemd1.BrokerTest", "Exit", &error, NULL, NULL));
        ASSERT_OK_ERRNO(pthread_join(server, &server_result));
        ASSERT_NULL(server_result);
}

static int connect_broker_socket(const char *socket_path, sd_bus **ret) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_close_ int fd = -EBADF;
        int r;

        assert(socket_path);
        assert(ret);

        fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
        if (fd < 0)
                return -errno;

        r = connect_unix_path(fd, AT_FDCWD, socket_path);
        if (r < 0)
                return r;

        r = sd_bus_new(&bus);
        if (r < 0)
                return r;

        r = sd_bus_set_fd(bus, fd, fd);
        if (r < 0)
                return r;
        TAKE_FD(fd);

        r = sd_bus_start(bus);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(bus);
        return 0;
}

TEST_RET(executable_socket_smoke) {
        _cleanup_(pidref_done_sigkill_wait) PidRef broker = PIDREF_NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmpdir = NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *build_root, *broker_path, *socket_path, *path;
        uint32_t job_id;
        siginfo_t si;
        int r;

        build_root = getenv("PROJECT_BUILD_ROOT");
        if (!build_root)
                return log_tests_skipped("PROJECT_BUILD_ROOT is not set");

        broker_path = strjoina(build_root, "/systemd1-broker");
        if (access(broker_path, X_OK) < 0)
                return log_tests_skipped_errno(errno, "%s is not executable", broker_path);

        ASSERT_OK(mkdtemp_malloc(NULL, &tmpdir));
        socket_path = strjoina(tmpdir, "/broker.sock");

        r = ASSERT_OK(pidref_safe_fork("(systemd1-broker)", FORK_DEATHSIG_SIGKILL|FORK_LOG, &broker));
        if (r == 0) {
                const char *socket_arg = strjoina("--socket=", socket_path);

                execl(broker_path, "systemd1-broker", socket_arg, NULL);
                _exit(EXIT_FAILURE);
        }

        for (unsigned i = 0; i < 200; i++) {
                r = is_socket(socket_path);
                if (r > 0)
                        break;
                if (r < 0 && r != -ENOENT)
                        return r;

                usleep_safe(10 * USEC_PER_MSEC);
        }
        ASSERT_OK_POSITIVE(is_socket(socket_path));

        ASSERT_OK(connect_broker_socket(socket_path, &bus));
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "LoadUnit", &error, &reply, "s", "alpha.service"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/alpha_2eservice");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "StartUnit", &error, &reply, "ss", "cold.service", "replace"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/job/1");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "ListJobs", &error, &reply, NULL));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "(usssoo)"));
        ASSERT_OK_ZERO(sd_bus_message_read(reply, "(usssoo)", &job_id, &path, &path, &path, &path, &path));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        ASSERT_OK(pidref_kill(&broker, SIGTERM));
        ASSERT_OK(pidref_wait_for_terminate(&broker, &si));
        ASSERT_EQ(si.si_code, CLD_KILLED);
        ASSERT_EQ(si.si_status, SIGTERM);
        pidref_done(&broker);

        return EXIT_SUCCESS;
}

TEST_RET(executable_bus_address_smoke) {
        _cleanup_(pidref_done_sigkill_wait) PidRef bus_daemon = PIDREF_NULL, broker = PIDREF_NULL;
        _cleanup_(rm_rf_physical_and_freep) char *tmpdir = NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *build_root, *broker_path, *socket_path, *bus_socket_path, *bus_address, *path;
        uint32_t job_id;
        siginfo_t si;
        int r;

        build_root = getenv("PROJECT_BUILD_ROOT");
        if (!build_root)
                return log_tests_skipped("PROJECT_BUILD_ROOT is not set");

        broker_path = strjoina(build_root, "/systemd1-broker");
        if (access(broker_path, X_OK) < 0)
                return log_tests_skipped_errno(errno, "%s is not executable", broker_path);

        if (access("/usr/bin/dbus-daemon", X_OK) < 0)
                return log_tests_skipped_errno(errno, "/usr/bin/dbus-daemon is not executable");

        ASSERT_OK(mkdtemp_malloc(NULL, &tmpdir));
        socket_path = strjoina(tmpdir, "/broker.sock");
        bus_socket_path = strjoina(tmpdir, "/bus.sock");
        bus_address = strjoina("unix:path=", bus_socket_path);

        r = ASSERT_OK(pidref_safe_fork("(dbus-daemon)", FORK_DEATHSIG_SIGKILL|FORK_LOG, &bus_daemon));
        if (r == 0) {
                const char *address_arg = strjoina("--address=", bus_address);

                execl("/usr/bin/dbus-daemon", "dbus-daemon", "--session", address_arg, "--nofork", "--nopidfile", NULL);
                _exit(EXIT_FAILURE);
        }

        for (unsigned i = 0; i < 200; i++) {
                r = is_socket(bus_socket_path);
                if (r > 0)
                        break;
                if (r < 0 && r != -ENOENT)
                        return r;

                usleep_safe(10 * USEC_PER_MSEC);
        }
        ASSERT_OK_POSITIVE(is_socket(bus_socket_path));

        r = ASSERT_OK(pidref_safe_fork("(systemd1-broker)", FORK_DEATHSIG_SIGKILL|FORK_LOG, &broker));
        if (r == 0) {
                const char *socket_arg = strjoina("--socket=", socket_path);
                const char *bus_address_arg = strjoina("--bus-address=", bus_address);

                execl(broker_path, "systemd1-broker", socket_arg, bus_address_arg, NULL);
                _exit(EXIT_FAILURE);
        }

        for (unsigned i = 0; i < 200; i++) {
                r = is_socket(socket_path);
                if (r > 0)
                        break;
                if (r < 0 && r != -ENOENT)
                        return r;

                usleep_safe(10 * USEC_PER_MSEC);
        }
        ASSERT_OK_POSITIVE(is_socket(socket_path));

        ASSERT_OK(sd_bus_new(&bus));
        ASSERT_OK(sd_bus_set_address(bus, bus_address));
        ASSERT_OK(sd_bus_set_bus_client(bus, true));
        ASSERT_OK(sd_bus_start(bus));

        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "LoadUnit", &error, &reply, "s", "alpha.service"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/alpha_2eservice");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "StartUnit", &error, &reply, "ss", "cold.service", "replace"));
        ASSERT_OK(sd_bus_message_read(reply, "o", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/job/1");

        reply = sd_bus_message_unref(reply);
        ASSERT_OK(sd_bus_call_method(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1", "org.freedesktop.systemd1.Manager", "ListJobs", &error, &reply, NULL));
        ASSERT_OK(sd_bus_message_enter_container(reply, 'a', "(usssoo)"));
        ASSERT_OK_ZERO(sd_bus_message_read(reply, "(usssoo)", &job_id, &path, &path, &path, &path, &path));
        ASSERT_OK(sd_bus_message_exit_container(reply));

        ASSERT_OK(pidref_kill(&broker, SIGTERM));
        ASSERT_OK(pidref_wait_for_terminate(&broker, &si));
        ASSERT_EQ(si.si_code, CLD_KILLED);
        ASSERT_EQ(si.si_status, SIGTERM);
        pidref_done(&broker);

        ASSERT_OK(pidref_kill(&bus_daemon, SIGTERM));
        ASSERT_OK(pidref_wait_for_terminate(&bus_daemon, &si));
        ASSERT_EQ(si.si_code, CLD_EXITED);
        ASSERT_EQ(si.si_status, EXIT_SUCCESS);
        pidref_done(&bus_daemon);

        return EXIT_SUCCESS;
}

TEST(job_path) {
        _cleanup_free_ char *path = NULL;

        ASSERT_OK(systemd1_broker_job_path(1, &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/job/1");

        path = mfree(path);

        ASSERT_OK(systemd1_broker_job_path(UINT32_MAX, &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/job/4294967295");
}

TEST(backend_state_to_unit_state) {
        ASSERT_STREQ(systemd1_broker_backend_state_to_active_state(SYSTEMD1_BROKER_BACKEND_ABSENT), "inactive");
        ASSERT_STREQ(systemd1_broker_backend_state_to_sub_state(SYSTEMD1_BROKER_BACKEND_ABSENT), "dead");

        ASSERT_STREQ(systemd1_broker_backend_state_to_active_state(SYSTEMD1_BROKER_BACKEND_STARTING), "activating");
        ASSERT_STREQ(systemd1_broker_backend_state_to_sub_state(SYSTEMD1_BROKER_BACKEND_STARTING), "start");

        ASSERT_STREQ(systemd1_broker_backend_state_to_active_state(SYSTEMD1_BROKER_BACKEND_RUNNING), "active");
        ASSERT_STREQ(systemd1_broker_backend_state_to_sub_state(SYSTEMD1_BROKER_BACKEND_RUNNING), "running");

        ASSERT_STREQ(systemd1_broker_backend_state_to_active_state(SYSTEMD1_BROKER_BACKEND_STOPPING), "deactivating");
        ASSERT_STREQ(systemd1_broker_backend_state_to_sub_state(SYSTEMD1_BROKER_BACKEND_STOPPING), "stop");

        ASSERT_STREQ(systemd1_broker_backend_state_to_active_state(SYSTEMD1_BROKER_BACKEND_STOPPED), "inactive");
        ASSERT_STREQ(systemd1_broker_backend_state_to_sub_state(SYSTEMD1_BROKER_BACKEND_STOPPED), "dead");

        ASSERT_STREQ(systemd1_broker_backend_state_to_active_state(SYSTEMD1_BROKER_BACKEND_FAILED), "failed");
        ASSERT_STREQ(systemd1_broker_backend_state_to_sub_state(SYSTEMD1_BROKER_BACKEND_FAILED), "failed");

        ASSERT_NULL(systemd1_broker_backend_state_to_active_state(_SYSTEMD1_BROKER_BACKEND_STATE_INVALID));
        ASSERT_NULL(systemd1_broker_backend_state_to_sub_state(_SYSTEMD1_BROKER_BACKEND_STATE_INVALID));
}

TEST(unit_new) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;

        ASSERT_OK(systemd1_broker_unit_new("demo.service", "Demo Service", &unit));

        ASSERT_STREQ(systemd1_broker_unit_name(unit), "demo.service");
        ASSERT_STREQ(systemd1_broker_unit_description(unit), "Demo Service");
        ASSERT_STREQ(systemd1_broker_unit_path(unit), "/org/freedesktop/systemd1/unit/demo_2eservice");
        ASSERT_STREQ(systemd1_broker_unit_active_state(unit), "inactive");
        ASSERT_STREQ(systemd1_broker_unit_sub_state(unit), "dead");
}

TEST(unit_new_rejects_invalid_name) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;

        ASSERT_ERROR(systemd1_broker_unit_new("not a unit", "Broken", &unit), EINVAL);
        ASSERT_NULL(unit);
}

TEST(unit_set_backend_state) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;

        ASSERT_OK(systemd1_broker_unit_new("demo.service", "Demo Service", &unit));
        ASSERT_OK(systemd1_broker_unit_set_backend_state(unit, SYSTEMD1_BROKER_BACKEND_RUNNING));

        ASSERT_STREQ(systemd1_broker_unit_active_state(unit), "active");
        ASSERT_STREQ(systemd1_broker_unit_sub_state(unit), "running");

        ASSERT_ERROR(systemd1_broker_unit_set_backend_state(unit, _SYSTEMD1_BROKER_BACKEND_STATE_INVALID), EINVAL);
        ASSERT_STREQ(systemd1_broker_unit_active_state(unit), "active");
        ASSERT_STREQ(systemd1_broker_unit_sub_state(unit), "running");
}

TEST(manager_add_get_unit) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerUnit *first, *second;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_EQ(systemd1_broker_manager_n_units(manager), 0u);
        ASSERT_NULL(systemd1_broker_manager_get_unit(manager, "alpha.service"));

        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", &first));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "beta.service", "Beta", &second));

        ASSERT_EQ(systemd1_broker_manager_n_units(manager), 2u);
        ASSERT_PTR_EQ(systemd1_broker_manager_get_unit(manager, "alpha.service"), first);
        ASSERT_PTR_EQ(systemd1_broker_manager_get_unit(manager, "beta.service"), second);
        ASSERT_PTR_EQ(systemd1_broker_manager_unit_at(manager, 0), first);
        ASSERT_PTR_EQ(systemd1_broker_manager_unit_at(manager, 1), second);
        ASSERT_NULL(systemd1_broker_manager_unit_at(manager, 2));
}

TEST(manager_rejects_duplicate_and_invalid_units) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerUnit *unit = NULL;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", &unit));

        ASSERT_ERROR(systemd1_broker_manager_add_unit(manager, "alpha.service", "Duplicate", &unit), EEXIST);
        ASSERT_ERROR(systemd1_broker_manager_add_unit(manager, "not a unit", "Broken", &unit), EINVAL);
        ASSERT_EQ(systemd1_broker_manager_n_units(manager), 1u);
        ASSERT_STREQ(systemd1_broker_unit_description(systemd1_broker_manager_get_unit(manager, "alpha.service")), "Alpha");
}

TEST(manager_sync_units_reconciles_complete_catalog) {
        static const Systemd1BrokerBackendUnit first[] = {
                { .size = sizeof(Systemd1BrokerBackendUnit), .id = "alpha.service", .description = "Alpha", .state = SYSTEMD1_BROKER_BACKEND_RUNNING },
                { .size = sizeof(Systemd1BrokerBackendUnit), .id = "beta.service", .description = "Beta", .state = SYSTEMD1_BROKER_BACKEND_STOPPED },
        };
        static const Systemd1BrokerBackendUnit second[] = {
                { .size = sizeof(Systemd1BrokerBackendUnit), .id = "alpha.service", .description = "Alpha Updated", .state = SYSTEMD1_BROKER_BACKEND_FAILED },
                { .size = sizeof(Systemd1BrokerBackendUnit), .id = "gamma.service", .description = "Gamma", .state = SYSTEMD1_BROKER_BACKEND_RUNNING },
        };
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        TestBackendContext context = {
                .units = first,
                .n_units = ELEMENTSOF(first),
        };
        const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);
        Systemd1BrokerUnitInfo info;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "ondemand.service", "On Demand", NULL));

        ASSERT_OK(systemd1_broker_manager_sync_units(manager));
        ASSERT_EQ(context.list_calls, 1u);
        ASSERT_EQ(systemd1_broker_manager_n_units(manager), 3u);
        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &info));
        ASSERT_STREQ(info.active_state, "active");

        context.units = second;
        context.n_units = ELEMENTSOF(second);
        ASSERT_OK(systemd1_broker_manager_sync_units(manager));
        ASSERT_EQ(context.list_calls, 2u);
        ASSERT_EQ(systemd1_broker_manager_n_units(manager), 3u);
        ASSERT_NULL(systemd1_broker_manager_get_unit(manager, "beta.service"));
        ASSERT_NOT_NULL(systemd1_broker_manager_get_unit(manager, "ondemand.service"));
        ASSERT_NOT_NULL(systemd1_broker_manager_get_unit(manager, "gamma.service"));
        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &info));
        ASSERT_STREQ(info.description, "Alpha Updated");
        ASSERT_STREQ(info.active_state, "failed");

        context.list_error = -EIO;
        ASSERT_ERROR(systemd1_broker_manager_sync_units(manager), EIO);
        ASSERT_NOT_NULL(systemd1_broker_manager_get_unit(manager, "gamma.service"));
}

TEST(manager_refresh_unit_snapshot_replaces_cache_atomically) {
        static const Systemd1BrokerBackendProperty first_properties[] = {
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "FragmentPath",
                        .signature = "s",
                        .value_json = "  \"/etc/systemd/system/demo.service\" ",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "MainPID",
                        .signature = "u",
                        .value_json = "4242",
                },
        };
        static const Systemd1BrokerBackendProperty second_properties[] = {
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "MainPID",
                        .signature = "u",
                        .value_json = "4343",
                },
        };
        static const Systemd1BrokerBackendUnitSnapshot first = {
                .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                .description = "Demo Updated",
                .properties = first_properties,
                .n_properties = ELEMENTSOF(first_properties),
        };
        static const Systemd1BrokerBackendUnitSnapshot second = {
                .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                .state = SYSTEMD1_BROKER_BACKEND_FAILED,
                .description = "Demo Failed",
                .properties = second_properties,
                .n_properties = ELEMENTSOF(second_properties),
        };
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        TestBackendContext context = {
                .snapshot = &first,
        };
        const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);
        const Systemd1BrokerProperty *property;
        Systemd1BrokerUnit *unit;
        bool changed;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "demo.service", "Demo", &unit));

        ASSERT_OK(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", &changed));
        ASSERT_TRUE(changed);
        ASSERT_EQ(context.snapshot_calls, 1u);
        ASSERT_EQ(context.snapshot_free_calls, 1u);
        ASSERT_STREQ(systemd1_broker_unit_description(unit), "Demo Updated");
        ASSERT_STREQ(systemd1_broker_unit_active_state(unit), "active");
        ASSERT_EQ(systemd1_broker_unit_n_properties(unit), 2u);
        ASSERT_EQ(systemd1_broker_unit_metadata_generation(unit), 1u);
        property = ASSERT_PTR(systemd1_broker_unit_find_property(
                                unit,
                                "org.freedesktop.systemd1.Unit",
                                "FragmentPath"));
        ASSERT_STREQ(systemd1_broker_property_signature(property), "s");
        ASSERT_STREQ(systemd1_broker_property_value_json(property), "\"/etc/systemd/system/demo.service\"");

        context.snapshot = &second;
        ASSERT_OK(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", &changed));
        ASSERT_TRUE(changed);
        ASSERT_EQ(context.snapshot_calls, 2u);
        ASSERT_EQ(context.snapshot_free_calls, 2u);
        ASSERT_STREQ(systemd1_broker_unit_description(unit), "Demo Failed");
        ASSERT_STREQ(systemd1_broker_unit_active_state(unit), "failed");
        ASSERT_EQ(systemd1_broker_unit_n_properties(unit), 1u);
        ASSERT_EQ(systemd1_broker_unit_metadata_generation(unit), 2u);
        ASSERT_NULL(systemd1_broker_unit_find_property(
                        unit,
                        "org.freedesktop.systemd1.Unit",
                        "FragmentPath"));
        property = ASSERT_PTR(systemd1_broker_unit_find_property(
                                unit,
                                "org.freedesktop.systemd1.Service",
                                "MainPID"));
        ASSERT_STREQ(systemd1_broker_property_value_json(property), "4343");

        ASSERT_OK(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", &changed));
        ASSERT_FALSE(changed);
        ASSERT_EQ(systemd1_broker_unit_metadata_generation(unit), 3u);

        context.snapshot_error = -EIO;
        ASSERT_ERROR(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", &changed), EIO);
        ASSERT_EQ(context.snapshot_calls, 4u);
        ASSERT_EQ(context.snapshot_free_calls, 3u);
        ASSERT_STREQ(systemd1_broker_unit_description(unit), "Demo Failed");
        ASSERT_STREQ(systemd1_broker_unit_active_state(unit), "failed");
        ASSERT_EQ(systemd1_broker_unit_metadata_generation(unit), 3u);
        ASSERT_STREQ(systemd1_broker_property_value_json(ASSERT_PTR(systemd1_broker_unit_find_property(
                                unit,
                                "org.freedesktop.systemd1.Service",
                                "MainPID"))), "4343");
}

TEST(manager_refresh_unit_snapshot_filters_unavailable_properties) {
        static const Systemd1BrokerBackendProperty properties[] = {
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "Id",
                        .signature = "s",
                        .value_json = "\"wrong.service\"",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "com.example.Unsupported",
                        .name = "VendorData",
                        .signature = "s",
                        .value_json = "\"ignored\"",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "SocketFD",
                        .signature = "h",
                        .value_json = "3",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "VariantFD",
                        .signature = "v",
                        .value_json = "{\"signature\":\"h\",\"value\":3}",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "MainPID",
                        .signature = "u",
                        .value_json = "1234",
                },
        };
        static const Systemd1BrokerBackendUnitSnapshot snapshot = {
                .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                .description = "Demo",
                .properties = properties,
                .n_properties = ELEMENTSOF(properties),
        };
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        TestBackendContext context = { .snapshot = &snapshot };
        const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);
        Systemd1BrokerUnit *unit;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "demo.service", "Demo", &unit));
        ASSERT_OK(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", NULL));

        ASSERT_EQ(systemd1_broker_unit_n_properties(unit), 1u);
        ASSERT_NULL(systemd1_broker_unit_find_property(unit, "org.freedesktop.systemd1.Unit", "Id"));
        ASSERT_NULL(systemd1_broker_unit_find_property(unit, "com.example.Unsupported", "VendorData"));
        ASSERT_NULL(systemd1_broker_unit_find_property(unit, "org.freedesktop.systemd1.Service", "SocketFD"));
        ASSERT_NULL(systemd1_broker_unit_find_property(unit, "org.freedesktop.systemd1.Service", "VariantFD"));
        ASSERT_NOT_NULL(systemd1_broker_unit_find_property(unit, "org.freedesktop.systemd1.Service", "MainPID"));
}

TEST(manager_refresh_unit_snapshot_accepts_composite_values) {
        static const Systemd1BrokerBackendProperty properties[] = {
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "StringArray",
                        .signature = "as",
                        .value_json = "[\"alpha\",\"beta\"]",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "PairValue",
                        .signature = "(us)",
                        .value_json = "[7,\"seven\"]",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "DictionaryValue",
                        .signature = "a{ss}",
                        .value_json = "[[\"key\",\"value\"]]",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "VariantValue",
                        .signature = "v",
                        .value_json = "{\"signature\":\"u\",\"value\":42}",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "ObjectPathValue",
                        .signature = "o",
                        .value_json = "\"/org/freedesktop/systemd1/unit/demo_2eservice\"",
                },
        };
        static const Systemd1BrokerBackendUnitSnapshot snapshot = {
                .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                .description = "Demo",
                .properties = properties,
                .n_properties = ELEMENTSOF(properties),
        };
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        TestBackendContext context = { .snapshot = &snapshot };
        const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);
        Systemd1BrokerUnit *unit;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "demo.service", "Demo", &unit));
        ASSERT_OK(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", NULL));

        ASSERT_EQ(systemd1_broker_unit_n_properties(unit), ELEMENTSOF(properties));
        for (size_t i = 0; i < ELEMENTSOF(properties); i++) {
                const Systemd1BrokerProperty *property;

                property = ASSERT_PTR(systemd1_broker_unit_find_property(unit, properties[i].interface, properties[i].name));
                ASSERT_STREQ(systemd1_broker_property_signature(property), properties[i].signature);
                ASSERT_STREQ(systemd1_broker_property_value_json(property), properties[i].value_json);
        }
}

TEST(property_append_value_preserves_typed_values) {
        static const Systemd1BrokerBackendProperty properties[] = {
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "ScalarValue",
                        .signature = "(ybnqiuxtd)",
                        .value_json = "[255,true,-2,3,-4,5,-6,7,1.5]",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "StringArray",
                        .signature = "as",
                        .value_json = "[\"alpha\",\"beta\"]",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "PairValue",
                        .signature = "(us)",
                        .value_json = "[7,\"seven\"]",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "DictionaryValue",
                        .signature = "a{ss}",
                        .value_json = "[[\"key\",\"value\"]]",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "VariantValue",
                        .signature = "v",
                        .value_json = "{\"signature\":\"u\",\"value\":42}",
                },
        };
        static const Systemd1BrokerBackendUnitSnapshot snapshot = {
                .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                .description = "Demo",
                .properties = properties,
                .n_properties = ELEMENTSOF(properties),
        };
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *message = NULL;
        _cleanup_strv_free_ char **strings = NULL;
        TestBackendContext context = { .snapshot = &snapshot };
        const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);
        Systemd1BrokerUnit *unit;
        const char *key, *pair_string;
        uint8_t byte;
        int boolean;
        int16_t int16;
        uint16_t uint16;
        int32_t int32;
        uint32_t uint32;
        int64_t int64;
        uint64_t uint64;
        double real;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "demo.service", "Demo", &unit));
        ASSERT_OK(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", NULL));

        ASSERT_OK(new_property_value_message(ASSERT_PTR(systemd1_broker_unit_find_property(
                                        unit, "org.freedesktop.systemd1.Unit", "ScalarValue")), &message));
        ASSERT_OK(sd_bus_message_read(message, "(ybnqiuxtd)",
                                      &byte, &boolean, &int16, &uint16, &int32, &uint32, &int64, &uint64, &real));
        ASSERT_EQ(byte, 255);
        ASSERT_TRUE(boolean);
        ASSERT_EQ(int16, -2);
        ASSERT_EQ(uint16, 3);
        ASSERT_EQ(int32, -4);
        ASSERT_EQ(uint32, 5u);
        ASSERT_EQ(int64, -6);
        ASSERT_EQ(uint64, 7u);
        ASSERT_TRUE(real > 1.49 && real < 1.51);

        message = sd_bus_message_unref(message);
        ASSERT_OK(new_property_value_message(ASSERT_PTR(systemd1_broker_unit_find_property(
                                        unit, "org.freedesktop.systemd1.Unit", "StringArray")), &message));
        ASSERT_OK(sd_bus_message_read_strv(message, &strings));
        ASSERT_TRUE(strv_equal(strings, STRV_MAKE("alpha", "beta")));

        message = sd_bus_message_unref(message);
        ASSERT_OK(new_property_value_message(ASSERT_PTR(systemd1_broker_unit_find_property(
                                        unit, "org.freedesktop.systemd1.Unit", "PairValue")), &message));
        ASSERT_OK(sd_bus_message_read(message, "(us)", &uint32, &pair_string));
        ASSERT_EQ(uint32, 7u);
        ASSERT_STREQ(pair_string, "seven");

        message = sd_bus_message_unref(message);
        ASSERT_OK(new_property_value_message(ASSERT_PTR(systemd1_broker_unit_find_property(
                                        unit, "org.freedesktop.systemd1.Unit", "DictionaryValue")), &message));
        ASSERT_OK(sd_bus_message_enter_container(message, 'a', "{ss}"));
        ASSERT_OK(sd_bus_message_read(message, "{ss}", &key, &pair_string));
        ASSERT_STREQ(key, "key");
        ASSERT_STREQ(pair_string, "value");
        ASSERT_OK_ZERO(sd_bus_message_read(message, "{ss}", &key, &pair_string));
        ASSERT_OK(sd_bus_message_exit_container(message));

        message = sd_bus_message_unref(message);
        ASSERT_OK(new_property_value_message(ASSERT_PTR(systemd1_broker_unit_find_property(
                                        unit, "org.freedesktop.systemd1.Service", "VariantValue")), &message));
        ASSERT_OK(sd_bus_message_enter_container(message, 'v', "u"));
        ASSERT_OK(sd_bus_message_read(message, "u", &uint32));
        ASSERT_EQ(uint32, 42u);
        ASSERT_OK(sd_bus_message_exit_container(message));
}

TEST(manager_refresh_unit_snapshot_rejects_duplicates_and_schema_conflicts) {
        static const Systemd1BrokerBackendProperty uint_property = {
                .size = sizeof(Systemd1BrokerBackendProperty),
                .interface = "org.freedesktop.systemd1.Service",
                .name = "DynamicValue",
                .signature = "u",
                .value_json = "1",
        };
        static const Systemd1BrokerBackendProperty string_property = {
                .size = sizeof(Systemd1BrokerBackendProperty),
                .interface = "org.freedesktop.systemd1.Service",
                .name = "DynamicValue",
                .signature = "s",
                .value_json = "\"one\"",
        };
        static const Systemd1BrokerBackendProperty duplicate_properties[] = {
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "DynamicValue",
                        .signature = "u",
                        .value_json = "1",
                },
                {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Service",
                        .name = "DynamicValue",
                        .signature = "u",
                        .value_json = "2",
                },
        };
        Systemd1BrokerBackendUnitSnapshot snapshot = {
                .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                .description = "Demo",
                .properties = &uint_property,
                .n_properties = 1,
        };
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        TestBackendContext context = { .snapshot = &snapshot };
        const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);
        Systemd1BrokerUnit *alpha, *beta;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", &alpha));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "beta.service", "Beta", &beta));
        ASSERT_OK(systemd1_broker_manager_refresh_unit_snapshot(manager, "alpha.service", NULL));

        snapshot.properties = &string_property;
        ASSERT_ERROR(systemd1_broker_manager_refresh_unit_snapshot(manager, "beta.service", NULL), EBADMSG);
        ASSERT_EQ(systemd1_broker_unit_n_properties(beta), 0u);
        ASSERT_EQ(systemd1_broker_unit_metadata_generation(beta), 0u);

        snapshot.properties = duplicate_properties;
        snapshot.n_properties = ELEMENTSOF(duplicate_properties);
        ASSERT_ERROR(systemd1_broker_manager_refresh_unit_snapshot(manager, "alpha.service", NULL), EEXIST);
        ASSERT_EQ(systemd1_broker_unit_n_properties(alpha), 1u);
        ASSERT_EQ(systemd1_broker_unit_metadata_generation(alpha), 1u);
}

TEST(manager_refresh_unit_snapshot_rejects_invalid_values_and_limits) {
        static const struct {
                const char *interface;
                const char *name;
                const char *signature;
                const char *value_json;
        } invalid[] = {
                { "not-an-interface", "Value", "u", "1" },
                { "org.freedesktop.systemd1.Unit", "not-a-member", "u", "1" },
                { "org.freedesktop.systemd1.Unit", "Value", "uu", "[1,2]" },
                { "org.freedesktop.systemd1.Unit", "Value", "u", "{" },
                { "org.freedesktop.systemd1.Unit", "Value", "s", "1" },
                { "org.freedesktop.systemd1.Unit", "Value", "u", "null" },
                { "org.freedesktop.systemd1.Unit", "Value", "u", "4294967296" },
                { "org.freedesktop.systemd1.Unit", "Value", "(us)", "[1]" },
                { "org.freedesktop.systemd1.Unit", "Value", "as", "[\"ok\",1]" },
        };

        for (size_t i = 0; i < ELEMENTSOF(invalid); i++) {
                _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
                const Systemd1BrokerBackendProperty property = {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = invalid[i].interface,
                        .name = invalid[i].name,
                        .signature = invalid[i].signature,
                        .value_json = invalid[i].value_json,
                };
                const Systemd1BrokerBackendUnitSnapshot snapshot = {
                        .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                        .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                        .description = "Demo",
                        .properties = &property,
                        .n_properties = 1,
                };
                TestBackendContext context = { .snapshot = &snapshot };
                const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);
                Systemd1BrokerUnit *unit;

                ASSERT_OK(systemd1_broker_manager_new(&manager));
                ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
                ASSERT_OK(systemd1_broker_manager_add_unit(manager, "demo.service", "Demo", &unit));
                ASSERT_LT(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", NULL), 0);
                ASSERT_EQ(context.snapshot_free_calls, 1u);
                ASSERT_EQ(systemd1_broker_unit_n_properties(unit), 0u);
                ASSERT_EQ(systemd1_broker_unit_metadata_generation(unit), 0u);
                ASSERT_STREQ(systemd1_broker_unit_description(unit), "Demo");
        }

        for (size_t n_properties = 257; n_properties > 0; n_properties = 0) {
                _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
                _cleanup_free_ Systemd1BrokerBackendProperty *properties = new0(Systemd1BrokerBackendProperty, n_properties);
                Systemd1BrokerBackendUnitSnapshot snapshot = {
                        .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                        .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                        .description = "Demo",
                        .properties = properties,
                        .n_properties = n_properties,
                };
                TestBackendContext context = { .snapshot = &snapshot };
                const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);

                ASSERT_NOT_NULL(properties);
                for (size_t i = 0; i < n_properties; i++)
                        properties[i] = (Systemd1BrokerBackendProperty) {
                                .size = sizeof(Systemd1BrokerBackendProperty),
                                .interface = "org.freedesktop.systemd1.Unit",
                                .name = "Value",
                                .signature = "u",
                                .value_json = "1",
                        };
                ASSERT_OK(systemd1_broker_manager_new(&manager));
                ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
                ASSERT_OK(systemd1_broker_manager_add_unit(manager, "demo.service", "Demo", NULL));
                ASSERT_ERROR(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", NULL), E2BIG);
        }

        {
                _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
                _cleanup_free_ char *value_json = new(char, 1024U * 1024U + 3U);
                Systemd1BrokerBackendProperty property = {
                        .size = sizeof(Systemd1BrokerBackendProperty),
                        .interface = "org.freedesktop.systemd1.Unit",
                        .name = "LargeValue",
                        .signature = "s",
                        .value_json = value_json,
                };
                Systemd1BrokerBackendUnitSnapshot snapshot = {
                        .size = sizeof(Systemd1BrokerBackendUnitSnapshot),
                        .state = SYSTEMD1_BROKER_BACKEND_RUNNING,
                        .description = "Demo",
                        .properties = &property,
                        .n_properties = 1,
                };
                TestBackendContext context = { .snapshot = &snapshot };
                const Systemd1BrokerBackendOps ops = TEST_BACKEND_OPS(&context);

                ASSERT_NOT_NULL(value_json);
                value_json[0] = '"';
                memset(value_json + 1, 'x', 1024U * 1024U);
                value_json[1024U * 1024U + 1U] = '"';
                value_json[1024U * 1024U + 2U] = 0;
                ASSERT_OK(systemd1_broker_manager_new(&manager));
                ASSERT_OK(systemd1_broker_manager_set_backend(manager, &ops));
                ASSERT_OK(systemd1_broker_manager_add_unit(manager, "demo.service", "Demo", NULL));
                ASSERT_ERROR(systemd1_broker_manager_refresh_unit_snapshot(manager, "demo.service", NULL), E2BIG);
        }
}

TEST(unit_info_matches_list_units_shape) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;
        Systemd1BrokerUnitInfo info = {};

        ASSERT_OK(systemd1_broker_unit_new("demo.service", "Demo Service", &unit));
        ASSERT_OK(systemd1_broker_unit_set_backend_state(unit, SYSTEMD1_BROKER_BACKEND_RUNNING));
        ASSERT_OK(systemd1_broker_unit_get_info(unit, &info));

        ASSERT_STREQ(info.id, "demo.service");
        ASSERT_STREQ(info.description, "Demo Service");
        ASSERT_STREQ(info.load_state, "loaded");
        ASSERT_STREQ(info.active_state, "active");
        ASSERT_STREQ(info.sub_state, "running");
        ASSERT_STREQ(info.following, "");
        ASSERT_STREQ(info.path, "/org/freedesktop/systemd1/unit/demo_2eservice");
        ASSERT_EQ(info.job_id, 0u);
        ASSERT_STREQ(info.job_type, "");
        ASSERT_STREQ(info.job_path, "/");
}

TEST(unit_properties_neutral_values) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;
        Systemd1BrokerUnitProperties properties = {};

        ASSERT_OK(systemd1_broker_unit_new("demo.service", "Demo Service", &unit));
        ASSERT_OK(systemd1_broker_unit_set_backend_state(unit, SYSTEMD1_BROKER_BACKEND_RUNNING));
        ASSERT_OK(systemd1_broker_unit_get_properties(unit, &properties));

        ASSERT_STREQ(properties.id, "demo.service");
        ASSERT_STREQ(properties.description, "Demo Service");
        ASSERT_STREQ(properties.load_state, "loaded");
        ASSERT_STREQ(properties.active_state, "active");
        ASSERT_STREQ(properties.sub_state, "running");
        ASSERT_STREQ(properties.fragment_path, "");
        ASSERT_STREQ(properties.source_path, "");
        ASSERT_NULL(properties.dropin_paths);
        ASSERT_STREQ(properties.unit_file_state, "");
        ASSERT_FALSE(properties.need_daemon_reload);
        ASSERT_EQ(properties.job_id, 0u);
        ASSERT_STREQ(properties.job_path, "/");
        ASSERT_NULL(properties.invocation_id);
        ASSERT_EQ(properties.invocation_id_size, 0u);
}

TEST(unit_properties_include_attached_job) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerUnitProperties properties = {};
        Systemd1BrokerUnit *unit;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", &unit));
        ASSERT_OK(systemd1_broker_manager_add_job(manager, "alpha.service", "start", NULL));

        ASSERT_OK(systemd1_broker_unit_get_properties(unit, &properties));
        ASSERT_EQ(properties.job_id, 1u);
        ASSERT_STREQ(properties.job_path, "/org/freedesktop/systemd1/job/1");
}

TEST(service_properties_neutral_values) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;
        Systemd1BrokerServiceProperties properties = {};

        ASSERT_OK(systemd1_broker_unit_new("demo.service", "Demo Service", &unit));
        ASSERT_OK(systemd1_broker_unit_get_service_properties(unit, &properties));

        ASSERT_EQ(properties.main_pid, 0u);
        ASSERT_EQ(properties.exec_main_pid, 0u);
        ASSERT_EQ(properties.control_pid, 0u);
        ASSERT_STREQ(properties.result, "success");
        ASSERT_STREQ(properties.status_text, "");
        ASSERT_EQ(properties.status_errno, 0);
        ASSERT_STREQ(properties.status_bus_error, "");
        ASSERT_STREQ(properties.status_varlink_error, "");
        ASSERT_EQ(properties.exec_main_start_timestamp, 0u);
        ASSERT_EQ(properties.exec_main_exit_timestamp, 0u);
        ASSERT_EQ(properties.exec_main_code, 0);
        ASSERT_EQ(properties.exec_main_status, 0);
        ASSERT_STREQ(properties.pid_file, "");
        ASSERT_STREQ(properties.log_namespace, "");
}

TEST(service_properties_reject_non_service_units) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;
        Systemd1BrokerServiceProperties properties = {};

        ASSERT_OK(systemd1_broker_unit_new("demo.target", "Demo Target", &unit));
        ASSERT_ERROR(systemd1_broker_unit_get_service_properties(unit, &properties), EOPNOTSUPP);
}

TEST(manager_get_unit_info) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerUnit *unit;
        Systemd1BrokerUnitInfo info = {};

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", &unit));
        ASSERT_OK(systemd1_broker_unit_set_backend_state(unit, SYSTEMD1_BROKER_BACKEND_FAILED));

        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &info));
        ASSERT_STREQ(info.id, "alpha.service");
        ASSERT_STREQ(info.description, "Alpha");
        ASSERT_STREQ(info.active_state, "failed");
        ASSERT_STREQ(info.sub_state, "failed");
        ASSERT_STREQ(info.path, "/org/freedesktop/systemd1/unit/alpha_2eservice");

        ASSERT_ERROR(systemd1_broker_manager_get_unit_info(manager, "missing.service", &info), ENOENT);
}

TEST(manager_unit_info_at) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerUnitInfo info = {};

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "beta.service", "Beta", NULL));

        ASSERT_OK(systemd1_broker_manager_unit_info_at(manager, 0, &info));
        ASSERT_STREQ(info.id, "alpha.service");

        ASSERT_OK(systemd1_broker_manager_unit_info_at(manager, 1, &info));
        ASSERT_STREQ(info.id, "beta.service");

        ASSERT_ERROR(systemd1_broker_manager_unit_info_at(manager, 2, &info), ENOENT);
}

TEST(manager_list_unit_infos) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        _cleanup_free_ Systemd1BrokerUnitInfo *infos = NULL;
        size_t n = SIZE_MAX;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "beta.service", "Beta", NULL));

        ASSERT_OK(systemd1_broker_manager_list_unit_infos(manager, NULL, NULL, &infos, &n));
        ASSERT_EQ(n, 2u);
        ASSERT_STREQ(infos[0].id, "alpha.service");
        ASSERT_STREQ(infos[1].id, "beta.service");
}

TEST(manager_list_unit_infos_filtered) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        _cleanup_free_ Systemd1BrokerUnitInfo *infos = NULL;
        Systemd1BrokerUnit *alpha, *beta, *gamma;
        const char *const active_states[] = { "active", NULL };
        const char *const running_states[] = { "running", NULL };
        const char *const failed_patterns[] = { "g*.service", NULL };
        const char *const failed_states[] = { "failed", NULL };
        size_t n = SIZE_MAX;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", &alpha));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "beta.service", "Beta", &beta));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "gamma.service", "Gamma", &gamma));
        ASSERT_OK(systemd1_broker_unit_set_backend_state(beta, SYSTEMD1_BROKER_BACKEND_RUNNING));
        ASSERT_OK(systemd1_broker_unit_set_backend_state(gamma, SYSTEMD1_BROKER_BACKEND_FAILED));

        ASSERT_OK(systemd1_broker_manager_list_unit_infos(manager, active_states, NULL, &infos, &n));
        ASSERT_EQ(n, 1u);
        ASSERT_STREQ(infos[0].id, "beta.service");

        infos = mfree(infos);
        ASSERT_OK(systemd1_broker_manager_list_unit_infos(manager, running_states, NULL, &infos, &n));
        ASSERT_EQ(n, 1u);
        ASSERT_STREQ(infos[0].id, "beta.service");

        infos = mfree(infos);
        ASSERT_OK(systemd1_broker_manager_list_unit_infos(manager, failed_states, failed_patterns, &infos, &n));
        ASSERT_EQ(n, 1u);
        ASSERT_STREQ(infos[0].id, "gamma.service");
}

TEST(manager_list_unit_infos_by_names) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        _cleanup_free_ Systemd1BrokerUnitInfo *infos = NULL;
        const char *const names[] = { "beta.service", "alpha.service", NULL };
        const char *const missing[] = { "missing.service", NULL };
        size_t n = SIZE_MAX;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "beta.service", "Beta", NULL));

        ASSERT_OK(systemd1_broker_manager_list_unit_infos_by_names(manager, names, &infos, &n));
        ASSERT_EQ(n, 2u);
        ASSERT_STREQ(infos[0].id, "beta.service");
        ASSERT_STREQ(infos[1].id, "alpha.service");

        infos = mfree(infos);
        ASSERT_ERROR(systemd1_broker_manager_list_unit_infos_by_names(manager, missing, &infos, &n), ENOENT);
        ASSERT_NULL(infos);
}

TEST(manager_get_and_load_unit_path) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerUnitInfo unit_info = {};
        TestBackendContext backend_context = {
                .status_state = SYSTEMD1_BROKER_BACKEND_RUNNING,
        };
        const Systemd1BrokerBackendOps backend_ops = TEST_BACKEND_OPS(&backend_context);
        const char *path = NULL;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &backend_ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));

        ASSERT_OK(systemd1_broker_manager_get_unit_path(manager, "alpha.service", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/alpha_2eservice");

        path = NULL;
        ASSERT_OK(systemd1_broker_manager_load_unit_path(manager, "alpha.service", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/alpha_2eservice");
        ASSERT_EQ(backend_context.status_calls, 1u);

        path = NULL;
        ASSERT_ERROR(systemd1_broker_manager_get_unit_path(manager, "missing.service", &path), ENOENT);
        ASSERT_OK(systemd1_broker_manager_load_unit_path(manager, "missing.service", &path));
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/missing_2eservice");
        ASSERT_EQ(backend_context.status_calls, 2u);
        ASSERT_STREQ(backend_context.last_unit_name, "missing.service");
        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "missing.service", &unit_info));
        ASSERT_STREQ(unit_info.active_state, "active");
        ASSERT_STREQ(unit_info.sub_state, "running");

        path = NULL;
        ASSERT_ERROR(systemd1_broker_manager_load_unit_path(manager, "not a unit", &path), EINVAL);
}

TEST(manager_add_job_updates_unit_and_list_jobs) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerJob *job;
        Systemd1BrokerUnitInfo unit_info = {};
        Systemd1BrokerJobInfo job_info = {};

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));

        ASSERT_OK(systemd1_broker_manager_add_job(manager, "alpha.service", "start", &job));
        ASSERT_EQ(systemd1_broker_manager_n_jobs(manager), 1u);

        ASSERT_OK(systemd1_broker_manager_job_info_at(manager, 0, &job_info));
        ASSERT_EQ(job_info.id, 1u);
        ASSERT_STREQ(job_info.unit_id, "alpha.service");
        ASSERT_STREQ(job_info.job_type, "start");
        ASSERT_STREQ(job_info.state, "waiting");
        ASSERT_STREQ(job_info.path, "/org/freedesktop/systemd1/job/1");
        ASSERT_STREQ(job_info.unit_path, "/org/freedesktop/systemd1/unit/alpha_2eservice");
        ASSERT_PTR_EQ(systemd1_broker_manager_get_job(manager, job_info.id), job);

        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &unit_info));
        ASSERT_EQ(unit_info.job_id, 1u);
        ASSERT_STREQ(unit_info.job_type, "start");
        ASSERT_STREQ(unit_info.job_path, "/org/freedesktop/systemd1/job/1");

        ASSERT_ERROR(systemd1_broker_manager_add_job(manager, "alpha.service", "stop", NULL), EBUSY);
        ASSERT_ERROR(systemd1_broker_manager_add_job(manager, "missing.service", "start", NULL), ENOENT);
}

TEST(manager_job_running_and_complete) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerUnitInfo unit_info = {};
        Systemd1BrokerJobInfo job_info = {};

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));
        ASSERT_OK(systemd1_broker_manager_add_job(manager, "alpha.service", "start", NULL));

        ASSERT_OK(systemd1_broker_manager_set_job_running(manager, 1));
        ASSERT_OK(systemd1_broker_manager_get_job_info(manager, 1, &job_info));
        ASSERT_STREQ(job_info.state, "running");

        ASSERT_OK(systemd1_broker_manager_complete_job(manager, 1));
        ASSERT_EQ(systemd1_broker_manager_n_jobs(manager), 0u);
        ASSERT_NULL(systemd1_broker_manager_get_job(manager, 1));
        ASSERT_ERROR(systemd1_broker_manager_job_info_at(manager, 0, &job_info), ENOENT);
        ASSERT_ERROR(systemd1_broker_manager_complete_job(manager, 1), ENOENT);

        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &unit_info));
        ASSERT_EQ(unit_info.job_id, 0u);
        ASSERT_STREQ(unit_info.job_type, "");
        ASSERT_STREQ(unit_info.job_path, "/");
}

TEST(manager_start_stop_restart_operations) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerJobInfo job_info = {};
        Systemd1BrokerUnitInfo unit_info = {};
        TestBackendContext backend_context = {
                .status_state = SYSTEMD1_BROKER_BACKEND_RUNNING,
        };
        const Systemd1BrokerBackendOps backend_ops = TEST_BACKEND_OPS(&backend_context);

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &backend_ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));

        ASSERT_OK(systemd1_broker_manager_refresh_unit_status(manager, "alpha.service"));
        ASSERT_EQ(backend_context.status_calls, 1u);
        ASSERT_STREQ(backend_context.last_unit_name, "alpha.service");
        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &unit_info));
        ASSERT_STREQ(unit_info.active_state, "active");
        ASSERT_STREQ(unit_info.sub_state, "running");

        ASSERT_OK(systemd1_broker_manager_start_unit(manager, "alpha.service", "replace", NULL));
        ASSERT_EQ(backend_context.start_calls, 1u);
        ASSERT_OK(systemd1_broker_manager_job_info_at(manager, 0, &job_info));
        ASSERT_STREQ(job_info.job_type, "start");
        ASSERT_OK(systemd1_broker_manager_complete_job(manager, job_info.id));
        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &unit_info));
        ASSERT_STREQ(unit_info.active_state, "active");
        ASSERT_STREQ(unit_info.sub_state, "running");
        ASSERT_EQ(systemd1_broker_manager_n_jobs(manager), 0u);

        ASSERT_OK(systemd1_broker_manager_stop_unit(manager, "alpha.service", "replace", NULL));
        ASSERT_EQ(backend_context.stop_calls, 1u);
        ASSERT_OK(systemd1_broker_manager_job_info_at(manager, 0, &job_info));
        ASSERT_STREQ(job_info.job_type, "stop");
        ASSERT_OK(systemd1_broker_manager_complete_job(manager, job_info.id));
        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &unit_info));
        ASSERT_STREQ(unit_info.active_state, "inactive");
        ASSERT_STREQ(unit_info.sub_state, "dead");

        ASSERT_OK(systemd1_broker_manager_restart_unit(manager, "alpha.service", "replace", NULL));
        ASSERT_EQ(backend_context.stop_calls, 2u);
        ASSERT_EQ(backend_context.start_calls, 2u);
        ASSERT_OK(systemd1_broker_manager_job_info_at(manager, 0, &job_info));
        ASSERT_STREQ(job_info.job_type, "restart");
        ASSERT_OK(systemd1_broker_manager_complete_job(manager, job_info.id));
        ASSERT_OK(systemd1_broker_manager_get_unit_info(manager, "alpha.service", &unit_info));
        ASSERT_STREQ(unit_info.active_state, "active");
        ASSERT_STREQ(unit_info.sub_state, "running");
}

TEST(manager_operation_modes_and_conflicts) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerJobInfo job_info = {};

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL));

        ASSERT_ERROR(systemd1_broker_manager_start_unit(manager, "alpha.service", "isolate", NULL), EOPNOTSUPP);
        ASSERT_OK(systemd1_broker_manager_start_unit(manager, "missing.service", "replace", NULL));
        ASSERT_NOT_NULL(systemd1_broker_manager_get_unit(manager, "missing.service"));
        ASSERT_OK(systemd1_broker_manager_job_info_at(manager, 0, &job_info));
        ASSERT_STREQ(job_info.unit_id, "missing.service");
        ASSERT_OK(systemd1_broker_manager_complete_job(manager, job_info.id));

        ASSERT_OK(systemd1_broker_manager_start_unit(manager, "alpha.service", "replace", NULL));
        ASSERT_ERROR(systemd1_broker_manager_stop_unit(manager, "alpha.service", "fail", NULL), EBUSY);
        ASSERT_ERROR(systemd1_broker_manager_stop_unit(manager, "alpha.service", "replace", NULL), EBUSY);
}

TEST(manager_reload_and_try_restart_operations) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        Systemd1BrokerJobInfo job_info = {};
        TestBackendContext backend_context = {
                .status_state = SYSTEMD1_BROKER_BACKEND_RUNNING,
        };
        const Systemd1BrokerBackendOps backend_ops = TEST_BACKEND_OPS(&backend_context);
        Systemd1BrokerUnit *unit;

        ASSERT_OK(systemd1_broker_manager_new(&manager));
        ASSERT_OK(systemd1_broker_manager_set_backend(manager, &backend_ops));
        ASSERT_OK(systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", &unit));

        ASSERT_OK(systemd1_broker_manager_reload_unit(manager, "alpha.service", "replace", NULL));
        ASSERT_EQ(backend_context.start_calls, 0u);
        ASSERT_EQ(backend_context.stop_calls, 0u);
        ASSERT_OK(systemd1_broker_manager_job_info_at(manager, 0, &job_info));
        ASSERT_STREQ(job_info.job_type, "reload");
        ASSERT_OK(systemd1_broker_manager_complete_job(manager, job_info.id));

        ASSERT_OK(systemd1_broker_manager_try_restart_unit(manager, "alpha.service", "replace", NULL));
        ASSERT_EQ(systemd1_broker_manager_n_jobs(manager), 0u);

        ASSERT_OK(systemd1_broker_unit_set_backend_state(unit, SYSTEMD1_BROKER_BACKEND_RUNNING));
        ASSERT_OK(systemd1_broker_manager_try_restart_unit(manager, "alpha.service", "replace", NULL));
        ASSERT_EQ(systemd1_broker_manager_n_jobs(manager), 1u);
        ASSERT_OK(systemd1_broker_manager_job_info_at(manager, 0, &job_info));
        ASSERT_STREQ(job_info.job_type, "restart");
        ASSERT_EQ(backend_context.stop_calls, 1u);
        ASSERT_EQ(backend_context.start_calls, 1u);
}

TEST(bus_append_unit_info) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        const char *id, *description, *load_state, *active_state, *sub_state, *following, *path, *job_type, *job_path;
        uint32_t job_id;
        Systemd1BrokerUnitInfo info = {
                .id = "demo.service",
                .description = "Demo Service",
                .load_state = "loaded",
                .active_state = "active",
                .sub_state = "running",
                .following = "",
                .path = "/org/freedesktop/systemd1/unit/demo_2eservice",
                .job_id = 7,
                .job_type = "start",
                .job_path = "/org/freedesktop/systemd1/job/7",
        };

        ASSERT_OK(new_method_return_message(&m));
        ASSERT_OK(sd_bus_message_open_container(m, 'a', "(ssssssouso)"));
        ASSERT_OK(systemd1_broker_bus_append_unit_info(m, &info));
        ASSERT_OK(sd_bus_message_close_container(m));
        ASSERT_OK(sd_bus_message_seal(m, 1, 0));
        ASSERT_OK(sd_bus_message_rewind(m, true));

        ASSERT_OK(sd_bus_message_enter_container(m, 'a', "(ssssssouso)"));
        ASSERT_OK(sd_bus_message_read(m, "(ssssssouso)", &id, &description, &load_state, &active_state, &sub_state, &following, &path, &job_id, &job_type, &job_path));
        ASSERT_STREQ(id, "demo.service");
        ASSERT_STREQ(description, "Demo Service");
        ASSERT_STREQ(load_state, "loaded");
        ASSERT_STREQ(active_state, "active");
        ASSERT_STREQ(sub_state, "running");
        ASSERT_STREQ(following, "");
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/unit/demo_2eservice");
        ASSERT_EQ(job_id, 7u);
        ASSERT_STREQ(job_type, "start");
        ASSERT_STREQ(job_path, "/org/freedesktop/systemd1/job/7");
        ASSERT_OK(sd_bus_message_exit_container(m));
}

TEST(bus_append_job_info) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        const char *unit_id, *job_type, *state, *path, *unit_path;
        uint32_t id;
        Systemd1BrokerJobInfo info = {
                .id = 7,
                .unit_id = "demo.service",
                .job_type = "start",
                .state = "running",
                .path = "/org/freedesktop/systemd1/job/7",
                .unit_path = "/org/freedesktop/systemd1/unit/demo_2eservice",
        };

        ASSERT_OK(new_method_return_message(&m));
        ASSERT_OK(sd_bus_message_open_container(m, 'a', "(usssoo)"));
        ASSERT_OK(systemd1_broker_bus_append_job_info(m, &info));
        ASSERT_OK(sd_bus_message_close_container(m));
        ASSERT_OK(sd_bus_message_seal(m, 1, 0));
        ASSERT_OK(sd_bus_message_rewind(m, true));

        ASSERT_OK(sd_bus_message_enter_container(m, 'a', "(usssoo)"));
        ASSERT_OK(sd_bus_message_read(m, "(usssoo)", &id, &unit_id, &job_type, &state, &path, &unit_path));
        ASSERT_EQ(id, 7u);
        ASSERT_STREQ(unit_id, "demo.service");
        ASSERT_STREQ(job_type, "start");
        ASSERT_STREQ(state, "running");
        ASSERT_STREQ(path, "/org/freedesktop/systemd1/job/7");
        ASSERT_STREQ(unit_path, "/org/freedesktop/systemd1/unit/demo_2eservice");
        ASSERT_OK(sd_bus_message_exit_container(m));
}

TEST(bus_append_info_rejects_missing_required_strings) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        Systemd1BrokerUnitInfo unit_info = {
                .id = "demo.service",
                .description = "Demo Service",
                .load_state = "loaded",
                .active_state = "active",
                .sub_state = "running",
                .following = "",
                .path = "/org/freedesktop/systemd1/unit/demo_2eservice",
                .job_type = "start",
                .job_path = "/org/freedesktop/systemd1/job/7",
        };
        Systemd1BrokerJobInfo job_info = {
                .id = 7,
                .unit_id = "demo.service",
                .job_type = "start",
                .state = "running",
                .path = "/org/freedesktop/systemd1/job/7",
                .unit_path = "/org/freedesktop/systemd1/unit/demo_2eservice",
        };

        ASSERT_OK(new_method_return_message(&m));

        unit_info.active_state = NULL;
        ASSERT_ERROR(systemd1_broker_bus_append_unit_info(m, &unit_info), EINVAL);

        job_info.unit_path = NULL;
        ASSERT_ERROR(systemd1_broker_bus_append_job_info(m, &job_info), EINVAL);
}

DEFINE_TEST_MAIN(LOG_INFO);
