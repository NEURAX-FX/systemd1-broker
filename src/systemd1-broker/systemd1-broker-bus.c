/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "systemd1-broker-bus.h"

int systemd1_broker_bus_append_unit_info(sd_bus_message *message, const Systemd1BrokerUnitInfo *info) {
        assert(message);
        assert(info);

        if (!info->id ||
            !info->description ||
            !info->load_state ||
            !info->active_state ||
            !info->sub_state ||
            !info->following ||
            !info->path ||
            !info->job_type ||
            !info->job_path)
                return -EINVAL;

        return sd_bus_message_append(
                        message,
                        "(ssssssouso)",
                        info->id,
                        info->description,
                        info->load_state,
                        info->active_state,
                        info->sub_state,
                        info->following,
                        info->path,
                        info->job_id,
                        info->job_type,
                        info->job_path);
}

int systemd1_broker_bus_append_job_info(sd_bus_message *message, const Systemd1BrokerJobInfo *info) {
        assert(message);
        assert(info);

        if (!info->unit_id ||
            !info->job_type ||
            !info->state ||
            !info->path ||
            !info->unit_path)
                return -EINVAL;

        return sd_bus_message_append(
                        message,
                        "(usssoo)",
                        info->id,
                        info->unit_id,
                        info->job_type,
                        info->state,
                        info->path,
                        info->unit_path);
}
