/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>

#include "alloc-util.h"
#include "dlfcn-util.h"
#include "errno-util.h"
#include "log.h"
#include "string-util.h"
#include "strv.h"
#include "systemd1-broker.h"
#include "systemd1-broker-metadata.h"
#include "unit-def.h"
#include "unit-name.h"
#include "utf8.h"

struct Systemd1BrokerUnit {
        Systemd1BrokerManager *manager;
        char *name;
        char *description;
        char *path;
        Systemd1BrokerBackendState backend_state;
        Systemd1BrokerJob *job;
        uint64_t catalog_generation;
        Systemd1BrokerProperty *properties;
        size_t n_properties;
        uint64_t metadata_generation;
};

struct Systemd1BrokerManager {
        Systemd1BrokerUnit **units;
        size_t n_units;
        Systemd1BrokerJob **jobs;
        size_t n_jobs;
        uint32_t next_job_id;
        uint64_t catalog_generation;
        Systemd1BrokerMetadataSchema *metadata_schema;
        void *backend_dl;
        Systemd1BrokerBackendOps backend_ops;
};

struct Systemd1BrokerJob {
        uint32_t id;
        char *type;
        char *path;
        Systemd1BrokerUnit *unit;
        Systemd1BrokerBackendState final_state;
        bool running;
};

static Systemd1BrokerJob* systemd1_broker_job_free(Systemd1BrokerJob *job) {
        if (!job)
                return NULL;

        free(job->type);
        free(job->path);

        return mfree(job);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(Systemd1BrokerJob*, systemd1_broker_job_free);

static int systemd1_broker_manager_add_job_internal(Systemd1BrokerManager *manager, const char *unit_name, const char *job_type, Systemd1BrokerBackendState final_state, Systemd1BrokerJob **ret);

int systemd1_broker_job_path(uint32_t id, char **ret) {
        _cleanup_free_ char *path = NULL;

        assert(ret);

        if (asprintf(&path, "/org/freedesktop/systemd1/job/%" PRIu32, id) < 0)
                return -ENOMEM;

        *ret = TAKE_PTR(path);
        return 0;
}

const char* systemd1_broker_backend_state_to_active_state(Systemd1BrokerBackendState state) {
        static const char *const table[_SYSTEMD1_BROKER_BACKEND_STATE_MAX] = {
                [SYSTEMD1_BROKER_BACKEND_ABSENT]   = "inactive",
                [SYSTEMD1_BROKER_BACKEND_STARTING] = "activating",
                [SYSTEMD1_BROKER_BACKEND_RUNNING]  = "active",
                [SYSTEMD1_BROKER_BACKEND_STOPPING] = "deactivating",
                [SYSTEMD1_BROKER_BACKEND_STOPPED]  = "inactive",
                [SYSTEMD1_BROKER_BACKEND_FAILED]   = "failed",
        };

        if (state < 0 || state >= _SYSTEMD1_BROKER_BACKEND_STATE_MAX)
                return NULL;

        return table[state];
}

const char* systemd1_broker_backend_state_to_sub_state(Systemd1BrokerBackendState state) {
        static const char *const table[_SYSTEMD1_BROKER_BACKEND_STATE_MAX] = {
                [SYSTEMD1_BROKER_BACKEND_ABSENT]   = "dead",
                [SYSTEMD1_BROKER_BACKEND_STARTING] = "start",
                [SYSTEMD1_BROKER_BACKEND_RUNNING]  = "running",
                [SYSTEMD1_BROKER_BACKEND_STOPPING] = "stop",
                [SYSTEMD1_BROKER_BACKEND_STOPPED]  = "dead",
                [SYSTEMD1_BROKER_BACKEND_FAILED]   = "failed",
        };

        if (state < 0 || state >= _SYSTEMD1_BROKER_BACKEND_STATE_MAX)
                return NULL;

        return table[state];
}

Systemd1BrokerManager* systemd1_broker_manager_free(Systemd1BrokerManager *manager) {
        if (!manager)
                return NULL;

        for (size_t i = 0; i < manager->n_units; i++)
                systemd1_broker_unit_free(manager->units[i]);

        for (size_t i = 0; i < manager->n_jobs; i++)
                systemd1_broker_job_free(manager->jobs[i]);

        safe_dlclose(manager->backend_dl);
        systemd1_broker_metadata_schema_free(manager->metadata_schema);
        free(manager->units);
        free(manager->jobs);

        return mfree(manager);
}

int systemd1_broker_manager_new(Systemd1BrokerManager **ret) {
        Systemd1BrokerManager *manager;

        assert(ret);

        manager = new0(Systemd1BrokerManager, 1);
        if (!manager)
                return -ENOMEM;

        manager->next_job_id = 1;

        *ret = manager;
        return 0;
}

static int systemd1_broker_backend_ops_verify(const Systemd1BrokerBackendOps *ops) {
        if (!ops || ops->size < sizeof(Systemd1BrokerBackendOps) || !ops->status || !ops->start || !ops->stop ||
            !ops->list_units || !ops->free_units || !ops->get_unit_snapshot || !ops->free_unit_snapshot)
                return -EINVAL;

        return 0;
}

typedef struct PreparedCatalogUnit {
        Systemd1BrokerUnit *existing;
        Systemd1BrokerUnit *new_unit;
        char *description;
        Systemd1BrokerBackendState state;
} PreparedCatalogUnit;

static void prepared_catalog_units_free(PreparedCatalogUnit *prepared, size_t n) {
        if (!prepared)
                return;

        for (size_t i = 0; i < n; i++) {
                systemd1_broker_unit_free(prepared[i].new_unit);
                free(prepared[i].description);
        }
        free(prepared);
}

int systemd1_broker_manager_sync_units(Systemd1BrokerManager *manager) {
        Systemd1BrokerBackendUnit *backend_units = NULL;
        PreparedCatalogUnit *prepared = NULL;
        size_t n_backend_units = 0, n_new = 0;
        uint64_t generation;
        int r;

        assert(manager);

        if (!manager->backend_ops.list_units || !manager->backend_ops.free_units)
                return 0;

        r = manager->backend_ops.list_units(manager->backend_ops.userdata, &backend_units, &n_backend_units);
        if (r < 0)
                return r;
        if (n_backend_units > 0 && !backend_units) {
                r = -EBADMSG;
                goto finish;
        }

        if (n_backend_units > 0) {
                prepared = new0(PreparedCatalogUnit, n_backend_units);
                if (!prepared) {
                        r = -ENOMEM;
                        goto finish;
                }
        }

        for (size_t i = 0; i < n_backend_units; i++) {
                const Systemd1BrokerBackendUnit *item = backend_units + i;

                if (item->size < sizeof(Systemd1BrokerBackendUnit) ||
                    !item->id || !unit_name_is_valid(item->id, UNIT_NAME_PLAIN|UNIT_NAME_INSTANCE) ||
                    !systemd1_broker_backend_state_to_active_state(item->state)) {
                        r = -EBADMSG;
                        goto finish;
                }
                for (size_t j = 0; j < i; j++)
                        if (streq(backend_units[j].id, item->id)) {
                                r = -EEXIST;
                                goto finish;
                        }

                prepared[i].existing = systemd1_broker_manager_get_unit(manager, item->id);
                prepared[i].state = item->state;
                if (prepared[i].existing) {
                        prepared[i].description = strdup(empty_to_null(item->description) ?: item->id);
                        if (!prepared[i].description) {
                                r = -ENOMEM;
                                goto finish;
                        }
                } else {
                        r = systemd1_broker_unit_new(item->id, empty_to_null(item->description) ?: item->id, &prepared[i].new_unit);
                        if (r < 0)
                                goto finish;
                        prepared[i].new_unit->backend_state = item->state;
                        n_new++;
                }
        }

        if (!GREEDY_REALLOC(manager->units, manager->n_units + n_new)) {
                r = -ENOMEM;
                goto finish;
        }
        if (manager->catalog_generation == UINT64_MAX) {
                r = -EOVERFLOW;
                goto finish;
        }
        generation = manager->catalog_generation + 1;

        for (size_t i = 0; i < n_backend_units; i++) {
                Systemd1BrokerUnit *unit;

                if (prepared[i].existing) {
                        unit = prepared[i].existing;
                        free_and_replace(unit->description, prepared[i].description);
                        unit->backend_state = prepared[i].state;
                } else {
                        unit = TAKE_PTR(prepared[i].new_unit);
                        unit->manager = manager;
                        manager->units[manager->n_units++] = unit;
                }
                unit->catalog_generation = generation;
        }

        for (size_t i = 0; i < manager->n_units; ) {
                Systemd1BrokerUnit *unit = manager->units[i];

                if (unit->catalog_generation == 0 || unit->catalog_generation == generation || unit->job) {
                        i++;
                        continue;
                }

                memmove(manager->units + i, manager->units + i + 1, sizeof(Systemd1BrokerUnit*) * (manager->n_units - i - 1));
                manager->n_units--;
                systemd1_broker_unit_free(unit);
        }
        manager->catalog_generation = generation;
        r = 0;

finish:
        prepared_catalog_units_free(prepared, n_backend_units);
        manager->backend_ops.free_units(manager->backend_ops.userdata, backend_units, n_backend_units);
        return r;
}

bool systemd1_broker_manager_has_synced_units(Systemd1BrokerManager *manager) {
        assert(manager);

        return manager->catalog_generation > 0;
}

int systemd1_broker_manager_set_backend(Systemd1BrokerManager *manager, const Systemd1BrokerBackendOps *ops) {
        int r;

        assert(manager);

        r = systemd1_broker_backend_ops_verify(ops);
        if (r < 0)
                return r;

        manager->backend_ops = *ops;
        return 0;
}

int systemd1_broker_manager_refresh_unit_snapshot(Systemd1BrokerManager *manager, const char *name, bool *ret_changed) {
        _cleanup_free_ char *description = NULL;
        Systemd1BrokerBackendUnitExtra extra = {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = name,
        };
        Systemd1BrokerBackendUnitSnapshot *snapshot = NULL;
        Systemd1BrokerMetadataSchema *schema = NULL, *old_schema;
        Systemd1BrokerProperty *properties = NULL, *old_properties;
        size_t n_properties = 0, old_n_properties;
        Systemd1BrokerUnit *unit;
        bool changed;
        int r;

        assert(manager);
        assert(name);

        unit = systemd1_broker_manager_get_unit(manager, name);
        if (!unit)
                return -ENOENT;
        if (!manager->backend_ops.get_unit_snapshot || !manager->backend_ops.free_unit_snapshot)
                return -EOPNOTSUPP;

        r = manager->backend_ops.get_unit_snapshot(manager->backend_ops.userdata, name, &extra, &snapshot);
        if (r < 0)
                return r;
        if (!snapshot || snapshot->size < sizeof(Systemd1BrokerBackendUnitSnapshot) ||
            !systemd1_broker_backend_state_to_active_state(snapshot->state) ||
            (snapshot->description && !utf8_is_valid(snapshot->description))) {
                r = -EBADMSG;
                goto finish;
        }
        if (unit->metadata_generation == UINT64_MAX) {
                r = -EOVERFLOW;
                goto finish;
        }

        if (!isempty(snapshot->description)) {
                description = strdup(snapshot->description);
                if (!description) {
                        r = -ENOMEM;
                        goto finish;
                }
        }
        r = systemd1_broker_metadata_prepare(
                        manager->metadata_schema,
                        snapshot,
                        &properties,
                        &n_properties,
                        &schema);
        if (r < 0)
                goto finish;

        changed = unit->backend_state != snapshot->state ||
                  (description && !streq(unit->description, description)) ||
                  !systemd1_broker_properties_equal(unit->properties, unit->n_properties, properties, n_properties);

        old_schema = manager->metadata_schema;
        manager->metadata_schema = TAKE_PTR(schema);
        old_properties = unit->properties;
        old_n_properties = unit->n_properties;
        unit->properties = TAKE_PTR(properties);
        unit->n_properties = n_properties;
        unit->backend_state = snapshot->state;
        if (description)
                free_and_replace(unit->description, description);
        unit->metadata_generation++;

        systemd1_broker_metadata_schema_free(old_schema);
        systemd1_broker_properties_free(old_properties, old_n_properties);
        if (ret_changed)
                *ret_changed = changed;
        r = 0;

finish:
        systemd1_broker_metadata_schema_free(schema);
        systemd1_broker_properties_free(properties, n_properties);
        manager->backend_ops.free_unit_snapshot(manager->backend_ops.userdata, snapshot);
        return r;
}

int systemd1_broker_manager_load_backend(Systemd1BrokerManager *manager, const char *path) {
        _cleanup_(dlclosep) void *dl = NULL;
        Systemd1BrokerBackendGetOps get_ops;
        const Systemd1BrokerBackendOps *ops;
        const char *dle = NULL;
        int r;

        assert(manager);
        assert(path);

        r = dlopen_safe(path, &dl, &dle);
        if (r < 0)
                return log_debug_errno(r, "Failed to load systemd1 broker backend '%s': %s", path, dle ?: STRERROR(r));

        get_ops = (Systemd1BrokerBackendGetOps) dlsym(dl, SYSTEMD1_BROKER_BACKEND_GET_OPS_SYMBOL);
        if (!get_ops)
                return log_debug_errno(SYNTHETIC_ERRNO(ELIBBAD), "Backend '%s' does not export %s: %s", path, SYSTEMD1_BROKER_BACKEND_GET_OPS_SYMBOL, dlerror());

        ops = get_ops();
        r = systemd1_broker_manager_set_backend(manager, ops);
        if (r < 0)
                return r;

        safe_dlclose(manager->backend_dl);
        manager->backend_dl = TAKE_PTR(dl);
        return 0;
}

int systemd1_broker_manager_add_unit(Systemd1BrokerManager *manager, const char *name, const char *description, Systemd1BrokerUnit **ret) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;
        int r;

        assert(manager);
        assert(name);

        if (!unit_name_is_valid(name, UNIT_NAME_PLAIN|UNIT_NAME_INSTANCE))
                return -EINVAL;

        if (systemd1_broker_manager_get_unit(manager, name))
                return -EEXIST;

        r = systemd1_broker_unit_new(name, description, &unit);
        if (r < 0)
                return r;

        unit->manager = manager;

        if (!GREEDY_REALLOC(manager->units, manager->n_units + 1))
                return -ENOMEM;

        manager->units[manager->n_units++] = unit;
        if (ret)
                *ret = unit;

        TAKE_PTR(unit);
        return 0;
}

Systemd1BrokerUnit* systemd1_broker_manager_get_unit(Systemd1BrokerManager *manager, const char *name) {
        assert(manager);
        assert(name);

        for (size_t i = 0; i < manager->n_units; i++)
                if (streq(systemd1_broker_unit_name(manager->units[i]), name))
                        return manager->units[i];

        return NULL;
}

int systemd1_broker_manager_get_unit_info(Systemd1BrokerManager *manager, const char *name, Systemd1BrokerUnitInfo *ret) {
        Systemd1BrokerUnit *unit;

        assert(manager);
        assert(name);
        assert(ret);

        unit = systemd1_broker_manager_get_unit(manager, name);
        if (!unit)
                return -ENOENT;

        return systemd1_broker_unit_get_info(unit, ret);
}

size_t systemd1_broker_manager_n_units(Systemd1BrokerManager *manager) {
        assert(manager);

        return manager->n_units;
}

Systemd1BrokerUnit* systemd1_broker_manager_unit_at(Systemd1BrokerManager *manager, size_t index) {
        assert(manager);

        if (index >= manager->n_units)
                return NULL;

        return manager->units[index];
}

int systemd1_broker_manager_unit_info_at(Systemd1BrokerManager *manager, size_t index, Systemd1BrokerUnitInfo *ret) {
        Systemd1BrokerUnit *unit;

        assert(manager);
        assert(ret);

        unit = systemd1_broker_manager_unit_at(manager, index);
        if (!unit)
                return -ENOENT;

        return systemd1_broker_unit_get_info(unit, ret);
}

static bool unit_info_matches_states(const Systemd1BrokerUnitInfo *info, const char* const* states) {
        if (strv_isempty((char**) states))
                return true;

        return strv_contains((char**) states, info->load_state) ||
               strv_contains((char**) states, info->active_state) ||
               strv_contains((char**) states, info->sub_state);
}

static bool unit_info_matches_patterns(const Systemd1BrokerUnitInfo *info, const char* const* patterns) {
        return strv_fnmatch_or_empty((char**) patterns, info->id, 0);
}

int systemd1_broker_manager_list_unit_infos(Systemd1BrokerManager *manager, const char* const* states, const char* const* patterns, Systemd1BrokerUnitInfo **ret, size_t *ret_n) {
        _cleanup_free_ Systemd1BrokerUnitInfo *infos = NULL;
        size_t n = 0;

        assert(manager);
        assert(ret);
        assert(ret_n);

        if (manager->n_units > 0) {
                infos = new(Systemd1BrokerUnitInfo, manager->n_units);
                if (!infos)
                        return -ENOMEM;
        }

        for (size_t i = 0; i < manager->n_units; i++) {
                Systemd1BrokerUnitInfo info;
                int r;

                r = systemd1_broker_unit_get_info(manager->units[i], &info);
                if (r < 0)
                        return r;

                if (!unit_info_matches_states(&info, states) || !unit_info_matches_patterns(&info, patterns))
                        continue;

                infos[n++] = info;
        }

        *ret = TAKE_PTR(infos);
        *ret_n = n;
        return 0;
}

int systemd1_broker_manager_list_unit_infos_by_names(Systemd1BrokerManager *manager, const char* const* names, Systemd1BrokerUnitInfo **ret, size_t *ret_n) {
        _cleanup_free_ Systemd1BrokerUnitInfo *infos = NULL;
        size_t n_names;

        assert(manager);
        assert(ret);
        assert(ret_n);

        n_names = strv_length((char**) names);
        if (n_names > 0) {
                infos = new(Systemd1BrokerUnitInfo, n_names);
                if (!infos)
                        return -ENOMEM;
        }

        for (size_t i = 0; i < n_names; i++) {
                Systemd1BrokerUnit *unit;
                int r;

                unit = systemd1_broker_manager_get_unit(manager, names[i]);
                if (!unit)
                        return -ENOENT;

                r = systemd1_broker_unit_get_info(unit, infos + i);
                if (r < 0)
                        return r;
        }

        *ret = TAKE_PTR(infos);
        *ret_n = n_names;
        return 0;
}

int systemd1_broker_manager_get_unit_path(Systemd1BrokerManager *manager, const char *name, const char **ret) {
        Systemd1BrokerUnit *unit;

        assert(manager);
        assert(name);
        assert(ret);

        unit = systemd1_broker_manager_get_unit(manager, name);
        if (!unit)
                return -ENOENT;

        *ret = unit->path;
        return 0;
}

int systemd1_broker_manager_load_unit_path(Systemd1BrokerManager *manager, const char *name, const char **ret) {
        Systemd1BrokerUnit *unit;
        int r;

        assert(manager);
        assert(name);
        assert(ret);

        unit = systemd1_broker_manager_get_unit(manager, name);
        if (!unit) {
                r = systemd1_broker_manager_add_unit(manager, name, name, &unit);
                if (r < 0)
                        return r;
        }

        r = systemd1_broker_manager_refresh_unit_status(manager, name);
        if (r < 0)
                return r;

        *ret = systemd1_broker_unit_path(unit);
        return 0;
}

static Systemd1BrokerBackendUnitExtra systemd1_broker_unit_backend_extra(Systemd1BrokerUnit *unit) {
        assert(unit);

        return (Systemd1BrokerBackendUnitExtra) {
                .size = sizeof(Systemd1BrokerBackendUnitExtra),
                .id = unit->name,
                .description = unit->description,
        };
}

int systemd1_broker_manager_refresh_unit_status(Systemd1BrokerManager *manager, const char *name) {
        Systemd1BrokerBackendUnitExtra extra;
        Systemd1BrokerBackendState state;
        Systemd1BrokerUnit *unit;
        int r;

        assert(manager);
        assert(name);

        unit = systemd1_broker_manager_get_unit(manager, name);
        if (!unit)
                return -ENOENT;

        if (!manager->backend_ops.status)
                return 0;

        extra = systemd1_broker_unit_backend_extra(unit);
        r = manager->backend_ops.status(manager->backend_ops.userdata, unit->name, &extra, &state);
        if (r < 0)
                return r;

        return systemd1_broker_unit_set_backend_state(unit, state);
}

int systemd1_broker_manager_add_job(Systemd1BrokerManager *manager, const char *unit_name, const char *job_type, Systemd1BrokerJob **ret) {
        assert(manager);
        assert(unit_name);
        assert(job_type);

        return systemd1_broker_manager_add_job_internal(manager, unit_name, job_type, _SYSTEMD1_BROKER_BACKEND_STATE_INVALID, ret);
}

static int systemd1_broker_manager_add_job_internal(Systemd1BrokerManager *manager, const char *unit_name, const char *job_type, Systemd1BrokerBackendState final_state, Systemd1BrokerJob **ret) {
        _cleanup_(systemd1_broker_job_freep) Systemd1BrokerJob *job = NULL;
        Systemd1BrokerUnit *unit;
        int r;

        assert(manager);
        assert(unit_name);
        assert(job_type);

        unit = systemd1_broker_manager_get_unit(manager, unit_name);
        if (!unit)
                return -ENOENT;
        if (unit->job)
                return -EBUSY;
        if (isempty(job_type))
                return -EINVAL;

        if (final_state == _SYSTEMD1_BROKER_BACKEND_STATE_INVALID)
                final_state = unit->backend_state;

        job = new0(Systemd1BrokerJob, 1);
        if (!job)
                return -ENOMEM;

        job->id = manager->next_job_id++;
        if (manager->next_job_id == 0)
                return -EOVERFLOW;

        job->type = strdup(job_type);
        if (!job->type)
                return -ENOMEM;

        r = systemd1_broker_job_path(job->id, &job->path);
        if (r < 0)
                return r;

        job->unit = unit;
        job->final_state = final_state;

        if (!GREEDY_REALLOC(manager->jobs, manager->n_jobs + 1))
                return -ENOMEM;

        unit->job = job;
        manager->jobs[manager->n_jobs++] = job;
        if (ret)
                *ret = job;

        TAKE_PTR(job);
        return 0;
}

static int systemd1_broker_manager_validate_job_mode(const char *mode) {
        if (STR_IN_SET(mode, "replace", "fail"))
                return 0;

        return -EOPNOTSUPP;
}

static int systemd1_broker_manager_prepare_operation(Systemd1BrokerManager *manager, const char *unit_name, const char *mode) {
        Systemd1BrokerUnit *unit;
        int r;

        assert(manager);
        assert(unit_name);
        assert(mode);

        r = systemd1_broker_manager_validate_job_mode(mode);
        if (r < 0)
                return r;

        unit = systemd1_broker_manager_get_unit(manager, unit_name);
        if (!unit)
                return -ENOENT;
        if (unit->job)
                return -EBUSY;

        return 0;
}

static int systemd1_broker_manager_call_backend(Systemd1BrokerManager *manager, const char *unit_name, int (*operation)(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra)) {
        Systemd1BrokerBackendUnitExtra extra;
        Systemd1BrokerUnit *unit;

        assert(manager);
        assert(unit_name);

        if (!operation)
                return 0;

        unit = systemd1_broker_manager_get_unit(manager, unit_name);
        if (!unit)
                return -ENOENT;

        extra = systemd1_broker_unit_backend_extra(unit);
        return operation(manager->backend_ops.userdata, unit_name, &extra);
}

int systemd1_broker_manager_start_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret) {
        int r;

        assert(manager);
        assert(unit_name);
        assert(mode);

        r = systemd1_broker_manager_validate_job_mode(mode);
        if (r < 0)
                return r;

        if (!systemd1_broker_manager_get_unit(manager, unit_name)) {
                r = systemd1_broker_manager_add_unit(manager, unit_name, unit_name, NULL);
                if (r < 0)
                        return r;
        }

        r = systemd1_broker_manager_prepare_operation(manager, unit_name, mode);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_call_backend(manager, unit_name, manager->backend_ops.start);
        if (r < 0)
                return r;

        return systemd1_broker_manager_add_job_internal(manager, unit_name, "start", SYSTEMD1_BROKER_BACKEND_RUNNING, ret);
}

int systemd1_broker_manager_stop_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret) {
        int r;

        assert(manager);

        r = systemd1_broker_manager_prepare_operation(manager, unit_name, mode);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_call_backend(manager, unit_name, manager->backend_ops.stop);
        if (r < 0)
                return r;

        return systemd1_broker_manager_add_job_internal(manager, unit_name, "stop", SYSTEMD1_BROKER_BACKEND_STOPPED, ret);
}

int systemd1_broker_manager_restart_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret) {
        int r;

        assert(manager);

        r = systemd1_broker_manager_prepare_operation(manager, unit_name, mode);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_call_backend(manager, unit_name, manager->backend_ops.stop);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_call_backend(manager, unit_name, manager->backend_ops.start);
        if (r < 0)
                return r;

        return systemd1_broker_manager_add_job_internal(manager, unit_name, "restart", SYSTEMD1_BROKER_BACKEND_RUNNING, ret);
}

int systemd1_broker_manager_reload_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret) {
        int r;

        assert(manager);
        assert(unit_name);
        assert(mode);

        r = systemd1_broker_manager_validate_job_mode(mode);
        if (r < 0)
                return r;

        return systemd1_broker_manager_add_job_internal(manager, unit_name, "reload", _SYSTEMD1_BROKER_BACKEND_STATE_INVALID, ret);
}

int systemd1_broker_manager_try_restart_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret) {
        Systemd1BrokerUnit *unit;
        int r;

        assert(manager);
        assert(unit_name);
        assert(mode);

        r = systemd1_broker_manager_validate_job_mode(mode);
        if (r < 0)
                return r;

        unit = systemd1_broker_manager_get_unit(manager, unit_name);
        if (!unit)
                return -ENOENT;

        if (!streq(systemd1_broker_unit_active_state(unit), "active")) {
                if (ret)
                        *ret = NULL;
                return 0;
        }

        if (unit->job)
                return -EBUSY;

        r = systemd1_broker_manager_call_backend(manager, unit_name, manager->backend_ops.stop);
        if (r < 0)
                return r;

        r = systemd1_broker_manager_call_backend(manager, unit_name, manager->backend_ops.start);
        if (r < 0)
                return r;

        return systemd1_broker_manager_add_job_internal(manager, unit_name, "restart", SYSTEMD1_BROKER_BACKEND_RUNNING, ret);
}

Systemd1BrokerJob* systemd1_broker_manager_get_job(Systemd1BrokerManager *manager, uint32_t id) {
        assert(manager);

        for (size_t i = 0; i < manager->n_jobs; i++)
                if (manager->jobs[i]->id == id)
                        return manager->jobs[i];

        return NULL;
}

int systemd1_broker_job_get_info(Systemd1BrokerJob *job, Systemd1BrokerJobInfo *ret) {
        assert(job);
        assert(ret);

        *ret = (Systemd1BrokerJobInfo) {
                .id = job->id,
                .unit_id = job->unit->name,
                .job_type = job->type,
                .state = job->running ? "running" : "waiting",
                .path = job->path,
                .unit_path = job->unit->path,
        };

        return 0;
}

int systemd1_broker_manager_get_job_info(Systemd1BrokerManager *manager, uint32_t id, Systemd1BrokerJobInfo *ret) {
        Systemd1BrokerJob *job;

        assert(manager);
        assert(ret);

        job = systemd1_broker_manager_get_job(manager, id);
        if (!job)
                return -ENOENT;

        return systemd1_broker_job_get_info(job, ret);
}

size_t systemd1_broker_manager_n_jobs(Systemd1BrokerManager *manager) {
        assert(manager);

        return manager->n_jobs;
}

int systemd1_broker_manager_job_info_at(Systemd1BrokerManager *manager, size_t index, Systemd1BrokerJobInfo *ret) {
        assert(manager);
        assert(ret);

        if (index >= manager->n_jobs)
                return -ENOENT;

        return systemd1_broker_job_get_info(manager->jobs[index], ret);
}

int systemd1_broker_manager_set_job_running(Systemd1BrokerManager *manager, uint32_t id) {
        Systemd1BrokerJob *job;

        assert(manager);

        job = systemd1_broker_manager_get_job(manager, id);
        if (!job)
                return -ENOENT;

        job->running = true;
        return 0;
}

int systemd1_broker_manager_complete_job(Systemd1BrokerManager *manager, uint32_t id) {
        assert(manager);

        for (size_t i = 0; i < manager->n_jobs; i++) {
                Systemd1BrokerJob *job = manager->jobs[i];

                if (job->id != id)
                        continue;

                job->unit->backend_state = job->final_state;
                job->unit->job = NULL;
                memmove(manager->jobs + i, manager->jobs + i + 1, sizeof(Systemd1BrokerJob*) * (manager->n_jobs - i - 1));
                manager->n_jobs--;
                systemd1_broker_job_free(job);
                return 0;
        }

        return -ENOENT;
}

Systemd1BrokerUnit* systemd1_broker_unit_free(Systemd1BrokerUnit *unit) {
        if (!unit)
                return NULL;

        free(unit->name);
        free(unit->description);
        free(unit->path);
        systemd1_broker_properties_free(unit->properties, unit->n_properties);

        return mfree(unit);
}

int systemd1_broker_unit_new(const char *name, const char *description, Systemd1BrokerUnit **ret) {
        _cleanup_(systemd1_broker_unit_freep) Systemd1BrokerUnit *unit = NULL;

        assert(name);
        assert(ret);

        if (!unit_name_is_valid(name, UNIT_NAME_PLAIN|UNIT_NAME_INSTANCE))
                return -EINVAL;

        unit = new0(Systemd1BrokerUnit, 1);
        if (!unit)
                return -ENOMEM;

        unit->name = strdup(name);
        if (!unit->name)
                return -ENOMEM;

        unit->description = strdup(strempty(description));
        if (!unit->description)
                return -ENOMEM;

        unit->path = unit_dbus_path_from_name(name);
        if (!unit->path)
                return -ENOMEM;

        unit->backend_state = SYSTEMD1_BROKER_BACKEND_ABSENT;

        *ret = TAKE_PTR(unit);
        return 0;
}

int systemd1_broker_unit_set_backend_state(Systemd1BrokerUnit *unit, Systemd1BrokerBackendState state) {
        assert(unit);

        if (!systemd1_broker_backend_state_to_active_state(state))
                return -EINVAL;

        unit->backend_state = state;
        return 0;
}

int systemd1_broker_unit_get_info(Systemd1BrokerUnit *unit, Systemd1BrokerUnitInfo *ret) {
        assert(unit);
        assert(ret);

        *ret = (Systemd1BrokerUnitInfo) {
                .id = unit->name,
                .description = unit->description,
                .load_state = "loaded",
                .active_state = systemd1_broker_unit_active_state(unit),
                .sub_state = systemd1_broker_unit_sub_state(unit),
                .following = "",
                .path = unit->path,
                .job_id = unit->job ? unit->job->id : 0,
                .job_type = unit->job ? unit->job->type : "",
                .job_path = unit->job ? unit->job->path : "/",
        };

        return 0;
}

int systemd1_broker_unit_get_properties(Systemd1BrokerUnit *unit, Systemd1BrokerUnitProperties *ret) {
        assert(unit);
        assert(ret);

        *ret = (Systemd1BrokerUnitProperties) {
                .id = unit->name,
                .description = unit->description,
                .load_state = "loaded",
                .active_state = systemd1_broker_unit_active_state(unit),
                .sub_state = systemd1_broker_unit_sub_state(unit),
                .fragment_path = "",
                .source_path = "",
                .dropin_paths = NULL,
                .unit_file_state = "",
                .need_daemon_reload = false,
                .job_id = unit->job ? unit->job->id : 0,
                .job_path = unit->job ? unit->job->path : "/",
                .invocation_id = NULL,
                .invocation_id_size = 0,
        };

        return 0;
}

int systemd1_broker_unit_get_service_properties(Systemd1BrokerUnit *unit, Systemd1BrokerServiceProperties *ret) {
        assert(unit);
        assert(ret);

        if (!endswith(unit->name, ".service"))
                return -EOPNOTSUPP;

        *ret = (Systemd1BrokerServiceProperties) {
                .main_pid = 0,
                .exec_main_pid = 0,
                .control_pid = 0,
                .result = "success",
                .status_text = "",
                .status_errno = 0,
                .status_bus_error = "",
                .status_varlink_error = "",
                .exec_main_start_timestamp = 0,
                .exec_main_exit_timestamp = 0,
                .exec_main_code = 0,
                .exec_main_status = 0,
                .pid_file = "",
                .log_namespace = "",
        };

        return 0;
}

const char* systemd1_broker_unit_name(Systemd1BrokerUnit *unit) {
        assert(unit);

        return unit->name;
}

const char* systemd1_broker_unit_description(Systemd1BrokerUnit *unit) {
        assert(unit);

        return unit->description;
}

const char* systemd1_broker_unit_path(Systemd1BrokerUnit *unit) {
        assert(unit);

        return unit->path;
}

const char* systemd1_broker_unit_active_state(Systemd1BrokerUnit *unit) {
        assert(unit);

        return systemd1_broker_backend_state_to_active_state(unit->backend_state);
}

const char* systemd1_broker_unit_sub_state(Systemd1BrokerUnit *unit) {
        assert(unit);

        return systemd1_broker_backend_state_to_sub_state(unit->backend_state);
}

Systemd1BrokerManager* systemd1_broker_unit_manager(Systemd1BrokerUnit *unit) {
        assert(unit);

        return unit->manager;
}

size_t systemd1_broker_unit_n_properties(Systemd1BrokerUnit *unit) {
        assert(unit);

        return unit->n_properties;
}

uint64_t systemd1_broker_unit_metadata_generation(Systemd1BrokerUnit *unit) {
        assert(unit);

        return unit->metadata_generation;
}

const Systemd1BrokerProperty* systemd1_broker_unit_property_at(Systemd1BrokerUnit *unit, size_t index) {
        assert(unit);

        return systemd1_broker_properties_at(unit->properties, unit->n_properties, index);
}

const Systemd1BrokerProperty* systemd1_broker_unit_find_property(
                Systemd1BrokerUnit *unit,
                const char *interface,
                const char *name) {

        assert(unit);
        assert(interface);
        assert(name);

        return systemd1_broker_properties_find(unit->properties, unit->n_properties, interface, name);
}
