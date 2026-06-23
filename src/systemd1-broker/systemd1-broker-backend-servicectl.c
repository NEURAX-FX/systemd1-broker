/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>

#include "systemd1-broker-backend-api.h"

extern char **environ;

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

        int r;

        if (!unit_name || !extra || extra->size < sizeof(Systemd1BrokerBackendUnitExtra) || !ret_state)
                return -EINVAL;

        r = run_servicectl("status", unit_name);
        if (r < 0) {
                *ret_state = SYSTEMD1_BROKER_BACKEND_FAILED;
                return 0;
        }

        *ret_state = SYSTEMD1_BROKER_BACKEND_RUNNING;
        return 0;
}

static int servicectl_start(void *userdata, const char *unit_name, const Systemd1BrokerBackendUnitExtra *extra) {
        if (!unit_name || !extra || extra->size < sizeof(Systemd1BrokerBackendUnitExtra))
                return -EINVAL;

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
        };

        return &ops;
}
