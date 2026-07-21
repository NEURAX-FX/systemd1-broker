/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>

#include "sd-bus-vtable.h"

#include "architecture.h"
#include "alloc-util.h"
#include "build.h"
#include "errno-util.h"
#include "fd-util.h"
#include "log.h"
#include "memstream-util.h"
#include "strv.h"
#include "systemd1-broker-bus.h"
#include "systemd1-broker-dbus.h"
#include "systemd1-broker-metadata.h"

static int reply_no_such_unit(sd_bus_error *ret_error, const char *name) {
        return sd_bus_error_setf(ret_error, "org.freedesktop.systemd1.NoSuchUnit", "Unit %s not found.", name);
}

static int reply_no_such_job(sd_bus_error *ret_error, uint32_t id) {
        return sd_bus_error_setf(ret_error, "org.freedesktop.systemd1.NoSuchJob", "Job %u does not exist.", (unsigned) id);
}

static int reply_job_error(sd_bus_error *ret_error, int r) {
        if (r == -EOPNOTSUPP)
                return sd_bus_error_set(ret_error, SD_BUS_ERROR_NOT_SUPPORTED, "Operation is not supported.");
        if (r == -EBUSY)
                return sd_bus_error_set(ret_error, "org.freedesktop.systemd1.TransactionIsDestructive", "Unit already has a conflicting job.");

        return r;
}

static int method_get_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        const char *name, *path;
        int r;

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_get_unit_path(manager, name, &path);
        if (r == -ENOENT)
                return reply_no_such_unit(ret_error, name);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, "o", path);
}

static int method_load_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        const char *name, *path;
        int r;

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_load_unit_path(manager, name, &path);
        if (r == -ENOENT)
                return reply_no_such_unit(ret_error, name);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, "o", path);
}

static int property_get_manager_string(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        const char *value;

        if (streq(property, "Version"))
                value = PROJECT_VERSION_STR;
        else if (streq(property, "Features"))
                value = systemd_features ?: "";
        else if (streq(property, "Architecture"))
                value = architecture_to_string(uname_architecture());
        else if (STR_IN_SET(property, "Virtualization", "ControlGroup"))
                value = "";
        else if (streq(property, "SystemState"))
                value = "running";
        else
                return -EINVAL;

        return sd_bus_message_append(reply, "s", value);
}

static int property_get_manager_u32(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        size_t n;

        if (streq(property, "NNames"))
                n = systemd1_broker_manager_n_units(manager);
        else if (streq(property, "NJobs"))
                n = systemd1_broker_manager_n_jobs(manager);
        else
                return -EINVAL;

        if (n > UINT32_MAX)
                return -EOVERFLOW;

        return sd_bus_message_append(reply, "u", (uint32_t) n);
}

static int property_get_manager_environment(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        return sd_bus_message_append_strv(reply, NULL);
}

static int method_list_units_common(
                sd_bus_message *message,
                void *userdata,
                sd_bus_error *ret_error,
                char **states,
                char **patterns,
                bool by_names) {

        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        _cleanup_free_ Systemd1BrokerUnitInfo *infos = NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        size_t n_infos;
        int r;

        r = systemd1_broker_manager_sync_units(manager);
        if (r < 0 && !systemd1_broker_manager_has_synced_units(manager))
                return r;

        r = by_names ?
                systemd1_broker_manager_list_unit_infos_by_names(
                                manager,
                                (const char* const*) patterns,
                                &infos,
                                &n_infos) :
                systemd1_broker_manager_list_unit_infos(
                                manager,
                                (const char* const*) states,
                                (const char* const*) patterns,
                                &infos,
                                &n_infos);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "(ssssssouso)");
        if (r < 0)
                return r;

        for (size_t i = 0; i < n_infos; i++) {
                r = systemd1_broker_bus_append_unit_info(reply, infos + i);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static int method_list_units(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return method_list_units_common(message, userdata, ret_error, NULL, NULL, false);
}

static int method_list_units_filtered(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        _cleanup_strv_free_ char **states = NULL;
        int r;

        r = sd_bus_message_read_strv(message, &states);
        if (r < 0)
                return r;

        return method_list_units_common(message, userdata, ret_error, states, NULL, false);
}

static int method_list_units_by_patterns(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        _cleanup_strv_free_ char **states = NULL;
        _cleanup_strv_free_ char **patterns = NULL;
        int r;

        r = sd_bus_message_read_strv(message, &states);
        if (r < 0)
                return r;

        r = sd_bus_message_read_strv(message, &patterns);
        if (r < 0)
                return r;

        return method_list_units_common(message, userdata, ret_error, states, patterns, false);
}

static int method_list_units_by_names(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        _cleanup_strv_free_ char **names = NULL;
        int r;

        r = sd_bus_message_read_strv(message, &names);
        if (r < 0)
                return r;

        return method_list_units_common(message, userdata, ret_error, NULL, names, true);
}

static int method_list_jobs(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        size_t n_jobs;
        int r;

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "(usssoo)");
        if (r < 0)
                return r;

        n_jobs = systemd1_broker_manager_n_jobs(manager);
        for (size_t i = 0; i < n_jobs; i++) {
                Systemd1BrokerJobInfo info;

                r = systemd1_broker_manager_job_info_at(manager, i, &info);
                if (r < 0)
                        return r;

                r = systemd1_broker_bus_append_job_info(reply, &info);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static int method_get_job(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        Systemd1BrokerJobInfo info;
        uint32_t id;
        int r;

        r = sd_bus_message_read(message, "u", &id);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_get_job_info(manager, id, &info);
        if (r == -ENOENT)
                return reply_no_such_job(ret_error, id);
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, "o", info.path);
}

static int method_cancel_job(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        uint32_t id;
        int r;

        r = sd_bus_message_read(message, "u", &id);
        if (r < 0)
                return r;

        if (!systemd1_broker_manager_get_job(manager, id))
                return reply_no_such_job(ret_error, id);

        return sd_bus_error_set(ret_error, SD_BUS_ERROR_NOT_SUPPORTED, "Job cancellation is not supported.");
}

int systemd1_broker_dbus_emit_job_new(sd_bus *bus, const Systemd1BrokerJobInfo *info) {
        assert(bus);
        assert(info);

        if (!info->path || !info->unit_id)
                return -EINVAL;

        return sd_bus_emit_signal(
                        bus,
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "JobNew",
                        "uos",
                        info->id,
                        info->path,
                        info->unit_id);
}

int systemd1_broker_dbus_emit_job_removed(sd_bus *bus, const Systemd1BrokerJobInfo *info, const char *result) {
        assert(bus);
        assert(info);
        assert(result);

        if (!info->path || !info->unit_id)
                return -EINVAL;

        return sd_bus_emit_signal(
                        bus,
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        "JobRemoved",
                        "uoss",
                        info->id,
                        info->path,
                        info->unit_id,
                        result);
}

typedef int (*ManagerUnitOperation)(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret);

static int reply_job_path(
                sd_bus_message *message,
                int r,
                const char *unit_name,
                Systemd1BrokerJob *job,
                Systemd1BrokerUnit *unit,
                sd_bus_error *ret_error) {

        Systemd1BrokerJobInfo info;

        if (r == -ENOENT)
                return reply_no_such_unit(ret_error, unit_name);
        if (r < 0)
                return reply_job_error(ret_error, r);

        if (!job)
                return sd_bus_reply_method_return(message, "o", "/");

        r = systemd1_broker_job_get_info(job, &info);
        if (r < 0)
                return r;

        r = systemd1_broker_dbus_emit_job_new(sd_bus_message_get_bus(message), &info);
        if (r < 0)
                return r;

        if (unit) {
                r = systemd1_broker_dbus_emit_unit_properties_changed(sd_bus_message_get_bus(message), unit);
                if (r < 0)
                        return r;
        }

        return sd_bus_reply_method_return(message, "o", info.path);
}

static int method_unit_operation(sd_bus_message *message, void *userdata, sd_bus_error *ret_error, ManagerUnitOperation operation) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        Systemd1BrokerJob *job = NULL;
        const char *name, *mode;
        int r;

        r = sd_bus_message_read(message, "ss", &name, &mode);
        if (r < 0)
                return r;

        r = operation(manager, name, mode, &job);
        return reply_job_path(message, r, name, job, job ? systemd1_broker_manager_get_unit(manager, name) : NULL, ret_error);
}

static int method_start_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return method_unit_operation(message, userdata, ret_error, systemd1_broker_manager_start_unit);
}

static int method_stop_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return method_unit_operation(message, userdata, ret_error, systemd1_broker_manager_stop_unit);
}

static int method_restart_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return method_unit_operation(message, userdata, ret_error, systemd1_broker_manager_restart_unit);
}

static int method_reload_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return method_unit_operation(message, userdata, ret_error, systemd1_broker_manager_reload_unit);
}

static int method_try_restart_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return method_unit_operation(message, userdata, ret_error, systemd1_broker_manager_try_restart_unit);
}

static int method_reload_or_restart_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return method_unit_operation(message, userdata, ret_error, systemd1_broker_manager_restart_unit);
}

static int method_noop(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return sd_bus_reply_method_return(message, NULL);
}

static int method_reset_failed_unit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        const char *name;
        int r;

        r = sd_bus_message_read(message, "s", &name);
        if (r < 0)
                return r;

        if (!systemd1_broker_manager_get_unit(manager, name))
                return reply_no_such_unit(ret_error, name);

        return sd_bus_reply_method_return(message, NULL);
}

static int find_unit(sd_bus *bus, const char *path, const char *interface, void *userdata, void **ret_found, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);

        for (size_t i = 0; i < systemd1_broker_manager_n_units(manager); i++) {
                Systemd1BrokerUnit *unit = ASSERT_PTR(systemd1_broker_manager_unit_at(manager, i));

                if (streq(systemd1_broker_unit_path(unit), path)) {
                        *ret_found = unit;
                        return 1;
                }
        }

        return 0;
}

static int open_property_entry(sd_bus_message *message, const char *name, const char *signature) {
        int r;

        r = sd_bus_message_open_container(message, 'e', "sv");
        if (r < 0)
                return r;
        r = sd_bus_message_append_basic(message, 's', name);
        if (r < 0)
                return r;
        return sd_bus_message_open_container(message, 'v', signature);
}

static int close_property_entry(sd_bus_message *message) {
        int r;

        r = sd_bus_message_close_container(message);
        if (r < 0)
                return r;
        return sd_bus_message_close_container(message);
}

static const char* core_property_signature(const char *name) {
        if (STR_IN_SET(name, "Id", "Description", "LoadState", "ActiveState", "SubState"))
                return "s";
        if (streq(name, "Names"))
                return "as";
        if (streq(name, "Job"))
                return "(uo)";
        return NULL;
}

static int append_core_property_value(sd_bus_message *message, Systemd1BrokerUnit *unit, const char *name) {
        Systemd1BrokerUnitProperties properties;
        int r;

        r = systemd1_broker_unit_get_properties(unit, &properties);
        if (r < 0)
                return r;

        if (streq(name, "Id"))
                return sd_bus_message_append(message, "s", properties.id);
        if (streq(name, "Names"))
                return sd_bus_message_append_strv(message, STRV_MAKE(properties.id));
        if (streq(name, "Description"))
                return sd_bus_message_append(message, "s", properties.description);
        if (streq(name, "LoadState"))
                return sd_bus_message_append(message, "s", properties.load_state);
        if (streq(name, "ActiveState"))
                return sd_bus_message_append(message, "s", properties.active_state);
        if (streq(name, "SubState"))
                return sd_bus_message_append(message, "s", properties.sub_state);
        if (streq(name, "Job"))
                return sd_bus_message_append(message, "(uo)", properties.job_id, properties.job_path);
        return -ENOENT;
}

static int append_core_property(sd_bus_message *message, Systemd1BrokerUnit *unit, const char *name) {
        const char *signature;
        int r;

        signature = core_property_signature(name);
        if (!signature)
                return -ENOENT;
        r = open_property_entry(message, name, signature);
        if (r < 0)
                return r;
        r = append_core_property_value(message, unit, name);
        if (r < 0)
                return r;
        return close_property_entry(message);
}

static int append_dynamic_property(sd_bus_message *message, const Systemd1BrokerProperty *property) {
        int r;

        r = open_property_entry(
                        message,
                        systemd1_broker_property_name(property),
                        systemd1_broker_property_signature(property));
        if (r < 0)
                return r;
        r = systemd1_broker_property_append_value(message, property);
        if (r < 0)
                return r;
        return close_property_entry(message);
}

typedef struct PropertyCopy {
        char *interface;
        char *name;
        char *signature;
        char *value_json;
} PropertyCopy;

static void property_copies_free(PropertyCopy *copies, size_t n_copies) {
        if (!copies)
                return;

        for (size_t i = 0; i < n_copies; i++) {
                free(copies[i].interface);
                free(copies[i].name);
                free(copies[i].signature);
                free(copies[i].value_json);
        }
        free(copies);
}

static int property_copies_new(Systemd1BrokerUnit *unit, PropertyCopy **ret, size_t *ret_n) {
        PropertyCopy *copies;
        size_t n;

        n = systemd1_broker_unit_n_properties(unit);
        if (n == 0) {
                *ret = NULL;
                *ret_n = 0;
                return 0;
        }

        copies = new0(PropertyCopy, n);
        if (!copies)
                return -ENOMEM;
        for (size_t i = 0; i < n; i++) {
                const Systemd1BrokerProperty *property = ASSERT_PTR(systemd1_broker_unit_property_at(unit, i));

                copies[i] = (PropertyCopy) {
                        .interface = strdup(systemd1_broker_property_interface(property)),
                        .name = strdup(systemd1_broker_property_name(property)),
                        .signature = strdup(systemd1_broker_property_signature(property)),
                        .value_json = strdup(systemd1_broker_property_value_json(property)),
                };
                if (!copies[i].interface || !copies[i].name || !copies[i].signature || !copies[i].value_json) {
                        property_copies_free(copies, n);
                        return -ENOMEM;
                }
        }

        *ret = copies;
        *ret_n = n;
        return 0;
}

static const PropertyCopy* property_copy_find(
                const PropertyCopy *copies,
                size_t n_copies,
                const char *interface,
                const char *name) {

        for (size_t i = 0; i < n_copies; i++)
                if (streq(copies[i].interface, interface) && streq(copies[i].name, name))
                        return copies + i;
        return NULL;
}

static int emit_properties_invalidated(sd_bus *bus, Systemd1BrokerUnit *unit, const char *interface, char **names) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *message = NULL;
        int r;

        if (strv_isempty(names))
                return 0;
        strv_sort_uniq(names);

        r = sd_bus_message_new_signal(
                        bus,
                        &message,
                        systemd1_broker_unit_path(unit),
                        "org.freedesktop.DBus.Properties",
                        "PropertiesChanged");
        if (r < 0)
                return r;
        r = sd_bus_message_append(message, "s", interface);
        if (r < 0)
                return r;
        r = sd_bus_message_open_container(message, 'a', "{sv}");
        if (r < 0)
                return r;
        r = sd_bus_message_close_container(message);
        if (r < 0)
                return r;
        r = sd_bus_message_append_strv(message, names);
        if (r < 0)
                return r;
        return sd_bus_send(bus, message, NULL);
}

static int invalidated_property_extend(char ***unit_names, char ***service_names, const char *interface, const char *name) {
        if (streq(interface, "org.freedesktop.systemd1.Unit"))
                return strv_extend(unit_names, name);
        if (streq(interface, "org.freedesktop.systemd1.Service"))
                return strv_extend(service_names, name);
        return -EINVAL;
}

static void refresh_unit_metadata(sd_bus *bus, Systemd1BrokerUnit *unit) {
        _cleanup_strv_free_ char **service_names = NULL, **unit_names = NULL;
        _cleanup_free_ char *old_description = NULL;
        PropertyCopy *old_properties = NULL;
        const char *old_active_state, *old_sub_state;
        size_t n_old_properties = 0;
        bool changed = false;
        int r;

        old_description = strdup(systemd1_broker_unit_description(unit));
        if (!old_description)
                return;
        old_active_state = systemd1_broker_unit_active_state(unit);
        old_sub_state = systemd1_broker_unit_sub_state(unit);
        r = property_copies_new(unit, &old_properties, &n_old_properties);
        if (r < 0)
                return;

        r = systemd1_broker_manager_refresh_unit_snapshot(
                        systemd1_broker_unit_manager(unit),
                        systemd1_broker_unit_name(unit),
                        &changed);
        if (r < 0) {
                if (r != -EOPNOTSUPP)
                        log_debug_errno(r, "Failed to refresh metadata for %s, using cached generation: %m", systemd1_broker_unit_name(unit));
                goto finish;
        }
        if (!changed || !bus)
                goto finish;

        for (size_t i = 0; i < n_old_properties; i++) {
                const Systemd1BrokerProperty *property;

                property = systemd1_broker_unit_find_property(unit, old_properties[i].interface, old_properties[i].name);
                if (property &&
                    streq(old_properties[i].signature, systemd1_broker_property_signature(property)) &&
                    streq(old_properties[i].value_json, systemd1_broker_property_value_json(property)))
                        continue;
                r = invalidated_property_extend(
                                &unit_names,
                                &service_names,
                                old_properties[i].interface,
                                old_properties[i].name);
                if (r < 0)
                        goto signal_error;
        }
        for (size_t i = 0; i < systemd1_broker_unit_n_properties(unit); i++) {
                const Systemd1BrokerProperty *property = ASSERT_PTR(systemd1_broker_unit_property_at(unit, i));

                if (property_copy_find(
                                old_properties,
                                n_old_properties,
                                systemd1_broker_property_interface(property),
                                systemd1_broker_property_name(property)))
                        continue;
                r = invalidated_property_extend(
                                &unit_names,
                                &service_names,
                                systemd1_broker_property_interface(property),
                                systemd1_broker_property_name(property));
                if (r < 0)
                        goto signal_error;
        }
        if (!streq(old_description, systemd1_broker_unit_description(unit))) {
                r = strv_extend(&unit_names, "Description");
                if (r < 0)
                        goto signal_error;
        }
        if (!streq(old_active_state, systemd1_broker_unit_active_state(unit))) {
                r = strv_extend(&unit_names, "ActiveState");
                if (r < 0)
                        goto signal_error;
        }
        if (!streq(old_sub_state, systemd1_broker_unit_sub_state(unit))) {
                r = strv_extend(&unit_names, "SubState");
                if (r < 0)
                        goto signal_error;
        }

        r = emit_properties_invalidated(bus, unit, "org.freedesktop.systemd1.Unit", unit_names);
        if (r < 0)
                goto signal_error;
        r = emit_properties_invalidated(bus, unit, "org.freedesktop.systemd1.Service", service_names);
        if (r < 0)
                goto signal_error;
        goto finish;

signal_error:
        log_debug_errno(r, "Failed to emit metadata invalidation for %s: %m", systemd1_broker_unit_name(unit));

finish:
        property_copies_free(old_properties, n_old_properties);
}

static bool unit_supports_interface(Systemd1BrokerUnit *unit, const char *interface) {
        if (streq(interface, "org.freedesktop.systemd1.Unit"))
                return true;
        if (streq(interface, "org.freedesktop.systemd1.Service"))
                return endswith(systemd1_broker_unit_name(unit), ".service");
        return false;
}

static int method_unit_properties_get_all(sd_bus_message *message, Systemd1BrokerUnit *unit, sd_bus_error *ret_error) {
        static const char * const core_properties[] = {
                "Id",
                "Names",
                "Description",
                "LoadState",
                "ActiveState",
                "SubState",
                "Job",
        };
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        const char *interface;
        bool all;
        int r;

        r = sd_bus_message_read(message, "s", &interface);
        if (r < 0)
                return r;
        all = isempty(interface);
        if (!all && !unit_supports_interface(unit, interface))
                return sd_bus_error_setf(ret_error, SD_BUS_ERROR_UNKNOWN_INTERFACE, "Unknown unit interface %s.", interface);

        refresh_unit_metadata(sd_bus_message_get_bus(message), unit);
        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;
        r = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (r < 0)
                return r;

        if (all || streq(interface, "org.freedesktop.systemd1.Unit"))
                FOREACH_ELEMENT(name, core_properties) {
                        r = append_core_property(reply, unit, *name);
                        if (r < 0)
                                return r;
                }

        for (size_t i = 0; i < systemd1_broker_unit_n_properties(unit); i++) {
                const Systemd1BrokerProperty *property = ASSERT_PTR(systemd1_broker_unit_property_at(unit, i));

                if (!all && !streq(interface, systemd1_broker_property_interface(property)))
                        continue;
                r = append_dynamic_property(reply, property);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;
        return sd_bus_send(NULL, reply, NULL);
}

static int reply_unit_property(
                sd_bus_message *message,
                Systemd1BrokerUnit *unit,
                const char *interface,
                const char *name,
                sd_bus_error *ret_error) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        const Systemd1BrokerProperty *property;
        const char *signature;
        int r;

        if (!unit_supports_interface(unit, interface))
                return sd_bus_error_setf(ret_error, SD_BUS_ERROR_UNKNOWN_INTERFACE, "Unknown unit interface %s.", interface);

        refresh_unit_metadata(sd_bus_message_get_bus(message), unit);
        property = systemd1_broker_unit_find_property(unit, interface, name);
        signature = streq(interface, "org.freedesktop.systemd1.Unit") ? core_property_signature(name) : NULL;
        if (!property && !signature && streq(interface, "org.freedesktop.systemd1.Unit") && streq(name, "FragmentPath"))
                signature = "s";
        if (!property && !signature && streq(interface, "org.freedesktop.systemd1.Unit") && streq(name, "DropInPaths"))
                signature = "as";
        if (!property && !signature)
                return sd_bus_error_setf(ret_error, SD_BUS_ERROR_UNKNOWN_PROPERTY, "Unknown property %s.%s.", interface, name);

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;
        r = sd_bus_message_open_container(reply, 'v', property ? systemd1_broker_property_signature(property) : signature);
        if (r < 0)
                return r;
        if (property)
                r = systemd1_broker_property_append_value(reply, property);
        else if (core_property_signature(name))
                r = append_core_property_value(reply, unit, name);
        else if (streq(name, "FragmentPath"))
                r = sd_bus_message_append(reply, "s", "");
        else
                r = sd_bus_message_append_strv(reply, NULL);
        if (r < 0)
                return r;
        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;
        return sd_bus_send(NULL, reply, NULL);
}

static int method_unit_properties_get(sd_bus_message *message, Systemd1BrokerUnit *unit, sd_bus_error *ret_error) {
        const char *interface, *name;
        int r;

        r = sd_bus_message_read(message, "ss", &interface, &name);
        if (r < 0)
                return r;
        return reply_unit_property(message, unit, interface, name, ret_error);
}

static int append_introspection_property(FILE *stream, const char *name, const char *signature) {
        return fprintf(stream, "    <property name=\"%s\" type=\"%s\" access=\"read\"/>\n", name, signature) < 0 ? -EIO : 0;
}

static int method_unit_introspect(sd_bus_message *message, Systemd1BrokerUnit *unit) {
        static const char * const core_properties[] = {
                "Id",
                "Names",
                "Description",
                "LoadState",
                "ActiveState",
                "SubState",
                "Job",
        };
        _cleanup_(memstream_done) MemStream memstream = {};
        _cleanup_free_ char *xml = NULL;
        FILE *stream;
        int r;

        refresh_unit_metadata(sd_bus_message_get_bus(message), unit);
        stream = memstream_init(&memstream);
        if (!stream)
                return -ENOMEM;
        if (fputs("<node>\n  <interface name=\"org.freedesktop.systemd1.Unit\">\n", stream) < 0)
                return -EIO;
        FOREACH_ELEMENT(name, core_properties) {
                r = append_introspection_property(stream, *name, ASSERT_PTR(core_property_signature(*name)));
                if (r < 0)
                        return r;
        }
        for (size_t i = 0; i < systemd1_broker_unit_n_properties(unit); i++) {
                const Systemd1BrokerProperty *property = ASSERT_PTR(systemd1_broker_unit_property_at(unit, i));

                if (!streq(systemd1_broker_property_interface(property), "org.freedesktop.systemd1.Unit"))
                        continue;
                r = append_introspection_property(
                                stream,
                                systemd1_broker_property_name(property),
                                systemd1_broker_property_signature(property));
                if (r < 0)
                        return r;
        }
        if (fputs("    <method name=\"Start\"><arg name=\"mode\" type=\"s\" direction=\"in\"/><arg name=\"job\" type=\"o\" direction=\"out\"/></method>\n"
                  "    <method name=\"Stop\"><arg name=\"mode\" type=\"s\" direction=\"in\"/><arg name=\"job\" type=\"o\" direction=\"out\"/></method>\n"
                  "    <method name=\"Restart\"><arg name=\"mode\" type=\"s\" direction=\"in\"/><arg name=\"job\" type=\"o\" direction=\"out\"/></method>\n"
                  "  </interface>\n",
                  stream) < 0)
                return -EIO;

        if (endswith(systemd1_broker_unit_name(unit), ".service")) {
                if (fputs("  <interface name=\"org.freedesktop.systemd1.Service\">\n", stream) < 0)
                        return -EIO;
                for (size_t i = 0; i < systemd1_broker_unit_n_properties(unit); i++) {
                        const Systemd1BrokerProperty *property = ASSERT_PTR(systemd1_broker_unit_property_at(unit, i));

                        if (!streq(systemd1_broker_property_interface(property), "org.freedesktop.systemd1.Service"))
                                continue;
                        r = append_introspection_property(
                                        stream,
                                        systemd1_broker_property_name(property),
                                        systemd1_broker_property_signature(property));
                        if (r < 0)
                                return r;
                }
                if (fputs("  </interface>\n", stream) < 0)
                        return -EIO;
        }
        if (fputs("  <interface name=\"org.freedesktop.DBus.Properties\">\n"
                  "    <method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>\n"
                  "    <method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>\n"
                  "    <method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>\n"
                  "    <signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>\n"
                  "  </interface>\n"
                  "  <interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method></interface>\n"
                  "</node>\n",
                  stream) < 0)
                return -EIO;
        r = memstream_finalize(&memstream, &xml, NULL);
        if (r < 0)
                return r;
        return sd_bus_reply_method_return(message, "s", xml);
}

static int unit_object_handler(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);
        Systemd1BrokerUnit *unit = NULL;
        int r;

        r = find_unit(
                        sd_bus_message_get_bus(message),
                        sd_bus_message_get_path(message),
                        sd_bus_message_get_interface(message),
                        manager,
                        (void**) &unit,
                        ret_error);
        if (r <= 0)
                return r;

        if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "GetAll"))
                return method_unit_properties_get_all(message, unit, ret_error);
        if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "Get"))
                return method_unit_properties_get(message, unit, ret_error);
        if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "Set"))
                return sd_bus_error_set(ret_error, SD_BUS_ERROR_PROPERTY_READ_ONLY, "Unit properties are read-only.");
        if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Introspectable", "Introspect"))
                return method_unit_introspect(message, unit);
        return 0;
}

static int property_get_unit_string(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerUnitProperties properties;
        const char *value;
        int r;

        r = systemd1_broker_unit_get_properties(unit, &properties);
        if (r < 0)
                return r;

        if (streq(property, "Id"))
                value = properties.id;
        else if (streq(property, "Description"))
                value = properties.description;
        else if (streq(property, "Following"))
                value = "";
        else if (streq(property, "LoadState"))
                value = properties.load_state;
        else if (streq(property, "ActiveState"))
                value = properties.active_state;
        else if (streq(property, "SubState"))
                value = properties.sub_state;
        else if (streq(property, "FragmentPath"))
                value = properties.fragment_path;
        else if (streq(property, "SourcePath"))
                value = properties.source_path;
        else if (streq(property, "UnitFileState"))
                value = properties.unit_file_state;
        else
                return -EINVAL;

        return sd_bus_message_append(reply, "s", value);
}

static int property_get_unit_names(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);

        return sd_bus_message_append_strv(reply, STRV_MAKE(systemd1_broker_unit_name(unit)));
}

static int property_get_empty_strv(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        return sd_bus_message_append_strv(reply, NULL);
}

static int property_get_need_daemon_reload(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerUnitProperties properties;
        int value, r;

        r = systemd1_broker_unit_get_properties(unit, &properties);
        if (r < 0)
                return r;

        value = properties.need_daemon_reload;
        return sd_bus_message_append(reply, "b", value);
}

static int property_get_job(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerUnitProperties properties;
        int r;

        r = systemd1_broker_unit_get_properties(unit, &properties);
        if (r < 0)
                return r;

        return sd_bus_message_append(reply, "(uo)", properties.job_id, properties.job_path);
}

static int property_get_invocation_id(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        return sd_bus_message_append_array(reply, 'y', NULL, 0);
}

int systemd1_broker_dbus_emit_unit_properties_changed(sd_bus *bus, Systemd1BrokerUnit *unit) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *message = NULL;
        Systemd1BrokerUnitProperties properties;
        int r;

        assert(bus);
        assert(unit);

        r = systemd1_broker_unit_get_properties(unit, &properties);
        if (r < 0)
                return r;

        r = sd_bus_message_new_signal(
                        bus,
                        &message,
                        systemd1_broker_unit_path(unit),
                        "org.freedesktop.DBus.Properties",
                        "PropertiesChanged");
        if (r < 0)
                return r;

        r = sd_bus_message_append(message, "s", "org.freedesktop.systemd1.Unit");
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(message, 'a', "{sv}");
        if (r < 0)
                return r;

        r = sd_bus_message_append(message, "{sv}", "ActiveState", "s", properties.active_state);
        if (r < 0)
                return r;

        r = sd_bus_message_append(message, "{sv}", "SubState", "s", properties.sub_state);
        if (r < 0)
                return r;

        r = sd_bus_message_append(message, "{sv}", "Job", "(uo)", properties.job_id, properties.job_path);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(message);
        if (r < 0)
                return r;

        r = sd_bus_message_append(message, "as", 0);
        if (r < 0)
                return r;

        return sd_bus_send(bus, message, NULL);
}

int systemd1_broker_dbus_complete_jobs(sd_bus *bus, Systemd1BrokerManager *manager) {
        int r;

        assert(manager);

        if (!bus)
                return 0;

        while (systemd1_broker_manager_n_jobs(manager) > 0) {
                _cleanup_free_ char *path = NULL, *unit_id = NULL;
                Systemd1BrokerJobInfo info;
                Systemd1BrokerUnit *unit;

                r = systemd1_broker_manager_job_info_at(manager, 0, &info);
                if (r < 0)
                        return r;

                path = strdup(info.path);
                unit_id = strdup(info.unit_id);
                if (!path || !unit_id)
                        return -ENOMEM;

                info.path = path;
                info.unit_id = unit_id;
                unit = systemd1_broker_manager_get_unit(manager, unit_id);

                r = systemd1_broker_manager_complete_job(manager, info.id);
                if (r < 0)
                        return r;

                if (unit) {
                        r = systemd1_broker_dbus_emit_unit_properties_changed(bus, unit);
                        if (r < 0)
                                return r;
                }

                r = systemd1_broker_dbus_emit_job_removed(bus, &info, "done");
                if (r < 0)
                        return r;
        }

        return 0;
}

static int method_unit_start(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerJob *job = NULL;
        const char *mode;
        int r;

        r = sd_bus_message_read(message, "s", &mode);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_start_unit(systemd1_broker_unit_manager(unit), systemd1_broker_unit_name(unit), mode, &job);
        return reply_job_path(message, r, systemd1_broker_unit_name(unit), job, unit, ret_error);
}

static int method_unit_stop(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerJob *job = NULL;
        const char *mode;
        int r;

        r = sd_bus_message_read(message, "s", &mode);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_stop_unit(systemd1_broker_unit_manager(unit), systemd1_broker_unit_name(unit), mode, &job);
        return reply_job_path(message, r, systemd1_broker_unit_name(unit), job, unit, ret_error);
}

static int method_unit_restart(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerJob *job = NULL;
        const char *mode;
        int r;

        r = sd_bus_message_read(message, "s", &mode);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_restart_unit(systemd1_broker_unit_manager(unit), systemd1_broker_unit_name(unit), mode, &job);
        return reply_job_path(message, r, systemd1_broker_unit_name(unit), job, unit, ret_error);
}

static const sd_bus_vtable unit_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Id", "s", property_get_unit_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Names", "as", property_get_unit_names, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Following", "s", property_get_unit_string, 0, 0),
        SD_BUS_PROPERTY("Requires", "as", property_get_empty_strv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Wants", "as", property_get_empty_strv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Conflicts", "as", property_get_empty_strv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Before", "as", property_get_empty_strv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("After", "as", property_get_empty_strv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Description", "s", property_get_unit_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LoadState", "s", property_get_unit_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ActiveState", "s", property_get_unit_string, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("SubState", "s", property_get_unit_string, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("FragmentPath", "s", property_get_unit_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SourcePath", "s", property_get_unit_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("DropInPaths", "as", property_get_empty_strv, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UnitFileState", "s", property_get_unit_string, 0, 0),
        SD_BUS_PROPERTY("NeedDaemonReload", "b", property_get_need_daemon_reload, 0, 0),
        SD_BUS_PROPERTY("Job", "(uo)", property_get_job, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("InvocationID", "ay", property_get_invocation_id, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_METHOD_WITH_ARGS("Start",
                                SD_BUS_ARGS("s", mode),
                                SD_BUS_RESULT("o", job),
                                method_unit_start,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("Stop",
                                SD_BUS_ARGS("s", mode),
                                SD_BUS_RESULT("o", job),
                                method_unit_stop,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("Restart",
                                SD_BUS_ARGS("s", mode),
                                SD_BUS_RESULT("o", job),
                                method_unit_restart,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
};

static int property_get_service_u32(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerServiceProperties properties;
        uint32_t value;
        int r;

        r = systemd1_broker_unit_get_service_properties(unit, &properties);
        if (r < 0)
                return r;

        if (streq(property, "MainPID"))
                value = properties.main_pid;
        else if (streq(property, "ExecMainPID"))
                value = properties.exec_main_pid;
        else if (streq(property, "ControlPID"))
                value = properties.control_pid;
        else
                return -EINVAL;

        return sd_bus_message_append(reply, "u", value);
}

static int property_get_service_string(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerServiceProperties properties;
        const char *value;
        int r;

        r = systemd1_broker_unit_get_service_properties(unit, &properties);
        if (r < 0)
                return r;

        if (streq(property, "Result"))
                value = properties.result;
        else if (streq(property, "StatusText"))
                value = properties.status_text;
        else if (streq(property, "StatusBusError"))
                value = properties.status_bus_error;
        else if (streq(property, "StatusVarlinkError"))
                value = properties.status_varlink_error;
        else if (streq(property, "PIDFile"))
                value = properties.pid_file;
        else if (streq(property, "LogNamespace"))
                value = properties.log_namespace;
        else
                return -EINVAL;

        return sd_bus_message_append(reply, "s", value);
}

static int property_get_service_i32(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerServiceProperties properties;
        int32_t value;
        int r;

        r = systemd1_broker_unit_get_service_properties(unit, &properties);
        if (r < 0)
                return r;

        if (streq(property, "StatusErrno"))
                value = properties.status_errno;
        else if (streq(property, "ExecMainCode"))
                value = properties.exec_main_code;
        else if (streq(property, "ExecMainStatus"))
                value = properties.exec_main_status;
        else
                return -EINVAL;

        return sd_bus_message_append(reply, "i", value);
}

static int property_get_service_u64(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerUnit *unit = ASSERT_PTR(userdata);
        Systemd1BrokerServiceProperties properties;
        uint64_t value;
        int r;

        r = systemd1_broker_unit_get_service_properties(unit, &properties);
        if (r < 0)
                return r;

        if (streq(property, "ExecMainStartTimestamp"))
                value = properties.exec_main_start_timestamp;
        else if (streq(property, "ExecMainExitTimestamp"))
                value = properties.exec_main_exit_timestamp;
        else
                return -EINVAL;

        return sd_bus_message_append(reply, "t", value);
}

static const sd_bus_vtable service_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("MainPID", "u", property_get_service_u32, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("ExecMainPID", "u", property_get_service_u32, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("ControlPID", "u", property_get_service_u32, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Result", "s", property_get_service_string, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("StatusText", "s", property_get_service_string, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("StatusErrno", "i", property_get_service_i32, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("StatusBusError", "s", property_get_service_string, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("StatusVarlinkError", "s", property_get_service_string, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("ExecMainStartTimestamp", "t", property_get_service_u64, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("ExecMainExitTimestamp", "t", property_get_service_u64, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("ExecMainCode", "i", property_get_service_i32, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("ExecMainStatus", "i", property_get_service_i32, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("PIDFile", "s", property_get_service_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogNamespace", "s", property_get_service_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_VTABLE_END,
};

static int find_service(sd_bus *bus, const char *path, const char *interface, void *userdata, void **ret_found, sd_bus_error *ret_error) {
        Systemd1BrokerUnit *unit;
        int r;

        r = find_unit(bus, path, interface, userdata, (void**) &unit, ret_error);
        if (r <= 0)
                return r;

        r = systemd1_broker_unit_get_service_properties(unit, &(Systemd1BrokerServiceProperties) {});
        if (r == -EOPNOTSUPP)
                return 0;
        if (r < 0)
                return r;

        *ret_found = unit;
        return 1;
}

static int find_job(sd_bus *bus, const char *path, const char *interface, void *userdata, void **ret_found, sd_bus_error *ret_error) {
        Systemd1BrokerManager *manager = ASSERT_PTR(userdata);

        for (size_t i = 0; i < systemd1_broker_manager_n_jobs(manager); i++) {
                Systemd1BrokerJobInfo info;
                int r;

                r = systemd1_broker_manager_job_info_at(manager, i, &info);
                if (r < 0)
                        return r;

                if (streq(info.path, path)) {
                        *ret_found = systemd1_broker_manager_get_job(manager, info.id);
                        return 1;
                }
        }

        return 0;
}

static int property_get_job_id(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerJob *job = ASSERT_PTR(userdata);
        Systemd1BrokerJobInfo info;
        int r;

        r = systemd1_broker_job_get_info(job, &info);
        if (r < 0)
                return r;

        return sd_bus_message_append(reply, "u", info.id);
}

static int property_get_job_string(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerJob *job = ASSERT_PTR(userdata);
        Systemd1BrokerJobInfo info;
        const char *value;
        int r;

        r = systemd1_broker_job_get_info(job, &info);
        if (r < 0)
                return r;

        if (streq(property, "JobType"))
                value = info.job_type;
        else if (streq(property, "State"))
                value = info.state;
        else
                return -EINVAL;

        return sd_bus_message_append(reply, "s", value);
}

static int property_get_job_unit(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        Systemd1BrokerJob *job = ASSERT_PTR(userdata);
        Systemd1BrokerJobInfo info;
        int r;

        r = systemd1_broker_job_get_info(job, &info);
        if (r < 0)
                return r;

        return sd_bus_message_append(reply, "(so)", info.unit_id, info.unit_path);
}

static int method_job_cancel(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        return sd_bus_error_set(ret_error, SD_BUS_ERROR_NOT_SUPPORTED, "Job cancellation is not supported.");
}

static const sd_bus_vtable job_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Id", "u", property_get_job_id, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Unit", "(so)", property_get_job_unit, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("JobType", "s", property_get_job_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("State", "s", property_get_job_string, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_METHOD("Cancel", NULL, NULL, method_job_cancel, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
};

static const sd_bus_vtable manager_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Version", "s", property_get_manager_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Features", "s", property_get_manager_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Virtualization", "s", property_get_manager_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Architecture", "s", property_get_manager_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NNames", "u", property_get_manager_u32, 0, 0),
        SD_BUS_PROPERTY("NJobs", "u", property_get_manager_u32, 0, 0),
        SD_BUS_PROPERTY("Environment", "as", property_get_manager_environment, 0, 0),
        SD_BUS_PROPERTY("ControlGroup", "s", property_get_manager_string, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SystemState", "s", property_get_manager_string, 0, 0),
        SD_BUS_METHOD_WITH_ARGS("GetUnit",
                                SD_BUS_ARGS("s", name),
                                SD_BUS_RESULT("o", unit),
                                method_get_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("LoadUnit",
                                SD_BUS_ARGS("s", name),
                                SD_BUS_RESULT("o", unit),
                                method_load_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("ListUnits",
                                SD_BUS_NO_ARGS,
                                SD_BUS_RESULT("a(ssssssouso)", units),
                                method_list_units,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("ListUnitsFiltered",
                                SD_BUS_ARGS("as", states),
                                SD_BUS_RESULT("a(ssssssouso)", units),
                                method_list_units_filtered,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("ListUnitsByPatterns",
                                SD_BUS_ARGS("as", states, "as", patterns),
                                SD_BUS_RESULT("a(ssssssouso)", units),
                                method_list_units_by_patterns,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("ListUnitsByNames",
                                SD_BUS_ARGS("as", names),
                                SD_BUS_RESULT("a(ssssssouso)", units),
                                method_list_units_by_names,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("ListJobs",
                                SD_BUS_NO_ARGS,
                                SD_BUS_RESULT("a(usssoo)", jobs),
                                method_list_jobs,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("GetJob",
                                SD_BUS_ARGS("u", id),
                                SD_BUS_RESULT("o", job),
                                method_get_job,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("CancelJob",
                                SD_BUS_ARGS("u", id),
                                SD_BUS_NO_RESULT,
                                method_cancel_job,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("StartUnit",
                                SD_BUS_ARGS("s", name, "s", mode),
                                SD_BUS_RESULT("o", job),
                                method_start_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("StopUnit",
                                SD_BUS_ARGS("s", name, "s", mode),
                                SD_BUS_RESULT("o", job),
                                method_stop_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("ReloadUnit",
                                SD_BUS_ARGS("s", name, "s", mode),
                                SD_BUS_RESULT("o", job),
                                method_reload_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("RestartUnit",
                                SD_BUS_ARGS("s", name, "s", mode),
                                SD_BUS_RESULT("o", job),
                                method_restart_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("TryRestartUnit",
                                SD_BUS_ARGS("s", name, "s", mode),
                                SD_BUS_RESULT("o", job),
                                method_try_restart_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("ReloadOrRestartUnit",
                                SD_BUS_ARGS("s", name, "s", mode),
                                SD_BUS_RESULT("o", job),
                                method_reload_or_restart_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("ResetFailedUnit",
                                SD_BUS_ARGS("s", name),
                                SD_BUS_NO_RESULT,
                                method_reset_failed_unit,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Subscribe", NULL, NULL, method_noop, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Unsubscribe", NULL, NULL, method_noop, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("ResetFailed", NULL, NULL, method_noop, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Reload", NULL, NULL, method_noop, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_SIGNAL_WITH_ARGS("JobNew",
                                SD_BUS_ARGS("u", id, "o", job, "s", unit),
                                0),
        SD_BUS_SIGNAL_WITH_ARGS("JobRemoved",
                                SD_BUS_ARGS("u", id, "o", job, "s", unit, "s", result),
                                0),
        SD_BUS_VTABLE_END,
};

int systemd1_broker_dbus_add_manager(sd_bus *bus, Systemd1BrokerManager *manager) {
        int r;

        assert(bus);
        assert(manager);

        r = sd_bus_add_object_vtable(
                        bus,
                        NULL,
                        "/org/freedesktop/systemd1",
                        "org.freedesktop.systemd1.Manager",
                        manager_vtable,
                        manager);
        if (r < 0)
                return r;

        r = sd_bus_add_fallback(
                        bus,
                        NULL,
                        "/org/freedesktop/systemd1/unit",
                        unit_object_handler,
                        manager);
        if (r < 0)
                return r;

        r = sd_bus_add_fallback_vtable(
                        bus,
                        NULL,
                        "/org/freedesktop/systemd1/unit",
                        "org.freedesktop.systemd1.Unit",
                        unit_vtable,
                        find_unit,
                        manager);
        if (r < 0)
                return r;

        r = sd_bus_add_fallback_vtable(
                        bus,
                        NULL,
                        "/org/freedesktop/systemd1/unit",
                        "org.freedesktop.systemd1.Service",
                        service_vtable,
                        find_service,
                        manager);
        if (r < 0)
                return r;

        return sd_bus_add_fallback_vtable(
                        bus,
                        NULL,
                        "/org/freedesktop/systemd1/job",
                        "org.freedesktop.systemd1.Job",
                        job_vtable,
                        find_job,
                        manager);
}

typedef struct BrokerTestContext {
        Systemd1BrokerManager *manager;
        bool quit;
} BrokerTestContext;

static int method_test_exit(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        BrokerTestContext *context = ASSERT_PTR(userdata);

        context->quit = true;
        return sd_bus_reply_method_return(message, NULL);
}

static int method_test_complete_job(sd_bus_message *message, void *userdata, sd_bus_error *ret_error) {
        BrokerTestContext *context = ASSERT_PTR(userdata);
        _cleanup_free_ char *path = NULL, *unit_id = NULL;
        Systemd1BrokerJobInfo info;
        Systemd1BrokerUnit *unit;
        uint32_t id;
        int r;

        r = sd_bus_message_read(message, "u", &id);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_get_job_info(context->manager, id, &info);
        if (r < 0)
                return r;

        unit = systemd1_broker_manager_get_unit(context->manager, info.unit_id);
        if (!unit)
                return -ENOENT;

        path = strdup(info.path);
        if (!path)
                return -ENOMEM;

        unit_id = strdup(info.unit_id);
        if (!unit_id)
                return -ENOMEM;

        info.path = path;
        info.unit_id = unit_id;

        r = systemd1_broker_manager_complete_job(context->manager, id);
        if (r < 0)
                return r;

        r = systemd1_broker_dbus_emit_unit_properties_changed(sd_bus_message_get_bus(message), unit);
        if (r < 0)
                return r;

        r = systemd1_broker_dbus_emit_job_removed(sd_bus_message_get_bus(message), &info, "done");
        if (r < 0)
                return r;

        return sd_bus_reply_method_return(message, NULL);
}

static const sd_bus_vtable test_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Exit", NULL, NULL, method_test_exit, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("CompleteJob",
                                SD_BUS_ARGS("u", id),
                                SD_BUS_NO_RESULT,
                                method_test_complete_job,
                                SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_VTABLE_END,
};

int systemd1_broker_serve_bus_fd_full(int fd, Systemd1BrokerManager *manager, bool add_test_api, bool *ret_quit) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        BrokerTestContext test_context = {
                .manager = manager,
        };
        sd_id128_t id;
        int r;

        assert(fd >= 0);
        assert(manager);

        r = sd_id128_randomize(&id);
        if (r < 0)
                return r;

        r = sd_bus_new(&bus);
        if (r < 0)
                return r;

        r = sd_bus_set_fd(bus, fd, fd);
        if (r < 0)
                return r;
        TAKE_FD(fd);

        r = sd_bus_set_server(bus, true, id);
        if (r < 0)
                return r;

        r = systemd1_broker_dbus_add_manager(bus, manager);
        if (r < 0)
                return r;

        if (add_test_api) {
                r = sd_bus_add_object_vtable(bus, NULL, "/broker/test", "org.freedesktop.systemd1.BrokerTest", test_vtable, &test_context);
                if (r < 0)
                        return r;
        }

        r = sd_bus_start(bus);
        if (r < 0)
                return r;

        while (!test_context.quit) {
                bool processed;

                r = sd_bus_process(bus, NULL);
                if (ERRNO_IS_NEG_DISCONNECT(r))
                        break;
                if (r < 0)
                        return r;
                processed = r > 0;

                if (!add_test_api) {
                        r = systemd1_broker_dbus_complete_jobs(bus, manager);
                        if (r < 0)
                                return r;
                }

                if (!processed) {
                        r = sd_bus_wait(bus, UINT64_MAX);
                        if (ERRNO_IS_NEG_DISCONNECT(r))
                                break;
                        if (r < 0)
                                return r;
                }
        }

        if (ret_quit)
                *ret_quit = test_context.quit;

        return 0;
}

int systemd1_broker_serve_bus_fd(int fd, Systemd1BrokerManager *manager, bool *ret_quit) {
        return systemd1_broker_serve_bus_fd_full(fd, manager, false, ret_quit);
}
