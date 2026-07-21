/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <errno.h>
#include <stddef.h>

typedef enum Systemd1BrokerBackendState {
        SYSTEMD1_BROKER_BACKEND_ABSENT,
        SYSTEMD1_BROKER_BACKEND_STARTING,
        SYSTEMD1_BROKER_BACKEND_RUNNING,
        SYSTEMD1_BROKER_BACKEND_STOPPING,
        SYSTEMD1_BROKER_BACKEND_STOPPED,
        SYSTEMD1_BROKER_BACKEND_FAILED,
        _SYSTEMD1_BROKER_BACKEND_STATE_MAX,
        _SYSTEMD1_BROKER_BACKEND_STATE_INVALID = -EINVAL,
} Systemd1BrokerBackendState;

typedef struct Systemd1BrokerBackendUnitExtra {
        size_t size;
        const char *id;
        const char *description;
        const char *backend_id;
        const char *type;
        const char *exec_start;
        const char *exec_stop;
        const char *environment;
        const char *cgroup;
        const char *dependencies;
        const char *target;
} Systemd1BrokerBackendUnitExtra;

typedef struct Systemd1BrokerBackendUnit {
        size_t size;
        const char *id;
        const char *description;
        Systemd1BrokerBackendState state;
} Systemd1BrokerBackendUnit;

typedef struct Systemd1BrokerBackendProperty {
        size_t size;
        const char *interface;
        const char *name;
        const char *signature;
        const char *value_json;
} Systemd1BrokerBackendProperty;

typedef struct Systemd1BrokerBackendUnitSnapshot {
        size_t size;
        Systemd1BrokerBackendState state;
        const char *description;
        const Systemd1BrokerBackendProperty *properties;
        size_t n_properties;
} Systemd1BrokerBackendUnitSnapshot;

typedef struct Systemd1BrokerBackendOps {
        size_t size;
        void *userdata;
        int (*status)(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra, Systemd1BrokerBackendState *ret_state);
        int (*start)(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra);
        int (*stop)(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra);
        int (*list_units)(void *userdata, Systemd1BrokerBackendUnit **ret_units, size_t *ret_n_units);
        void (*free_units)(void *userdata, Systemd1BrokerBackendUnit *units, size_t n_units);
        int (*get_unit_snapshot)(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra, Systemd1BrokerBackendUnitSnapshot **ret_snapshot);
        void (*free_unit_snapshot)(void *userdata, Systemd1BrokerBackendUnitSnapshot *snapshot);
} Systemd1BrokerBackendOps;

#define SYSTEMD1_BROKER_BACKEND_GET_OPS_SYMBOL "systemd1_broker_backend_get_ops"

typedef const Systemd1BrokerBackendOps* (*Systemd1BrokerBackendGetOps)(void);

const Systemd1BrokerBackendOps* systemd1_broker_backend_get_ops(void);
