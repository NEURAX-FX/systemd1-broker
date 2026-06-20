/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "basic-forward.h"

typedef enum Systemd1BrokerBackendState {
        SYSTEMD1_BROKER_BACKEND_ABSENT,
        SYSTEMD1_BROKER_BACKEND_STARTING,
        SYSTEMD1_BROKER_BACKEND_RUNNING,
        SYSTEMD1_BROKER_BACKEND_RELOADING,
        SYSTEMD1_BROKER_BACKEND_STOPPING,
        SYSTEMD1_BROKER_BACKEND_STOPPED,
        SYSTEMD1_BROKER_BACKEND_FAILED,
        _SYSTEMD1_BROKER_BACKEND_STATE_MAX,
        _SYSTEMD1_BROKER_BACKEND_STATE_INVALID = -EINVAL,
} Systemd1BrokerBackendState;

typedef struct Systemd1BrokerUnit Systemd1BrokerUnit;
typedef struct Systemd1BrokerManager Systemd1BrokerManager;
typedef struct Systemd1BrokerJob Systemd1BrokerJob;

typedef struct Systemd1BrokerUnitInfo {
        const char *id;
        const char *description;
        const char *load_state;
        const char *active_state;
        const char *sub_state;
        const char *following;
        const char *path;
        uint32_t job_id;
        const char *job_type;
        const char *job_path;
} Systemd1BrokerUnitInfo;

typedef struct Systemd1BrokerJobInfo {
        uint32_t id;
        const char *unit_id;
        const char *job_type;
        const char *state;
        const char *path;
        const char *unit_path;
} Systemd1BrokerJobInfo;

typedef struct Systemd1BrokerUnitProperties {
        const char *id;
        const char *description;
        const char *load_state;
        const char *active_state;
        const char *sub_state;
        const char *fragment_path;
        const char *source_path;
        const char* const *dropin_paths;
        const char *unit_file_state;
        bool need_daemon_reload;
        uint32_t job_id;
        const char *job_path;
        const uint8_t *invocation_id;
        size_t invocation_id_size;
} Systemd1BrokerUnitProperties;

typedef struct Systemd1BrokerServiceProperties {
        uint32_t main_pid;
        uint32_t exec_main_pid;
        uint32_t control_pid;
        const char *result;
        const char *status_text;
        int32_t status_errno;
        const char *status_bus_error;
        const char *status_varlink_error;
        uint64_t exec_main_start_timestamp;
        uint64_t exec_main_exit_timestamp;
        int32_t exec_main_code;
        int32_t exec_main_status;
        const char *pid_file;
        const char *log_namespace;
} Systemd1BrokerServiceProperties;

int systemd1_broker_job_path(uint32_t id, char **ret);
const char* systemd1_broker_backend_state_to_active_state(Systemd1BrokerBackendState state);
const char* systemd1_broker_backend_state_to_sub_state(Systemd1BrokerBackendState state);

Systemd1BrokerManager* systemd1_broker_manager_free(Systemd1BrokerManager *manager);
int systemd1_broker_manager_new(Systemd1BrokerManager **ret);
int systemd1_broker_manager_add_unit(Systemd1BrokerManager *manager, const char *name, const char *description, Systemd1BrokerUnit **ret);
Systemd1BrokerUnit* systemd1_broker_manager_get_unit(Systemd1BrokerManager *manager, const char *name);
int systemd1_broker_manager_get_unit_info(Systemd1BrokerManager *manager, const char *name, Systemd1BrokerUnitInfo *ret);
size_t systemd1_broker_manager_n_units(Systemd1BrokerManager *manager);
Systemd1BrokerUnit* systemd1_broker_manager_unit_at(Systemd1BrokerManager *manager, size_t index);
int systemd1_broker_manager_unit_info_at(Systemd1BrokerManager *manager, size_t index, Systemd1BrokerUnitInfo *ret);
int systemd1_broker_manager_list_unit_infos(Systemd1BrokerManager *manager, const char* const* states, const char* const* patterns, Systemd1BrokerUnitInfo **ret, size_t *ret_n);
int systemd1_broker_manager_list_unit_infos_by_names(Systemd1BrokerManager *manager, const char* const* names, Systemd1BrokerUnitInfo **ret, size_t *ret_n);
int systemd1_broker_manager_get_unit_path(Systemd1BrokerManager *manager, const char *name, const char **ret);
int systemd1_broker_manager_load_unit_path(Systemd1BrokerManager *manager, const char *name, const char **ret);
int systemd1_broker_manager_add_job(Systemd1BrokerManager *manager, const char *unit_name, const char *job_type, Systemd1BrokerJob **ret);
int systemd1_broker_manager_start_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret);
int systemd1_broker_manager_stop_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret);
int systemd1_broker_manager_restart_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret);
int systemd1_broker_manager_reload_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret);
int systemd1_broker_manager_try_restart_unit(Systemd1BrokerManager *manager, const char *unit_name, const char *mode, Systemd1BrokerJob **ret);
Systemd1BrokerJob* systemd1_broker_manager_get_job(Systemd1BrokerManager *manager, uint32_t id);
int systemd1_broker_manager_get_job_info(Systemd1BrokerManager *manager, uint32_t id, Systemd1BrokerJobInfo *ret);
size_t systemd1_broker_manager_n_jobs(Systemd1BrokerManager *manager);
int systemd1_broker_manager_job_info_at(Systemd1BrokerManager *manager, size_t index, Systemd1BrokerJobInfo *ret);
int systemd1_broker_manager_set_job_running(Systemd1BrokerManager *manager, uint32_t id);
int systemd1_broker_manager_complete_job(Systemd1BrokerManager *manager, uint32_t id);

Systemd1BrokerUnit* systemd1_broker_unit_free(Systemd1BrokerUnit *unit);
int systemd1_broker_unit_new(const char *name, const char *description, Systemd1BrokerUnit **ret);
int systemd1_broker_unit_set_backend_state(Systemd1BrokerUnit *unit, Systemd1BrokerBackendState state);
int systemd1_broker_unit_get_info(Systemd1BrokerUnit *unit, Systemd1BrokerUnitInfo *ret);
int systemd1_broker_unit_get_properties(Systemd1BrokerUnit *unit, Systemd1BrokerUnitProperties *ret);
int systemd1_broker_unit_get_service_properties(Systemd1BrokerUnit *unit, Systemd1BrokerServiceProperties *ret);

const char* systemd1_broker_unit_name(Systemd1BrokerUnit *unit);
const char* systemd1_broker_unit_description(Systemd1BrokerUnit *unit);
const char* systemd1_broker_unit_path(Systemd1BrokerUnit *unit);
const char* systemd1_broker_unit_active_state(Systemd1BrokerUnit *unit);
const char* systemd1_broker_unit_sub_state(Systemd1BrokerUnit *unit);
Systemd1BrokerManager* systemd1_broker_unit_manager(Systemd1BrokerUnit *unit);
int systemd1_broker_job_get_info(Systemd1BrokerJob *job, Systemd1BrokerJobInfo *ret);

DEFINE_TRIVIAL_CLEANUP_FUNC(Systemd1BrokerManager*, systemd1_broker_manager_free);
DEFINE_TRIVIAL_CLEANUP_FUNC(Systemd1BrokerUnit*, systemd1_broker_unit_free);
