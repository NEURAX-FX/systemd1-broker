/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "sd-bus.h"

#include "systemd1-broker.h"

int systemd1_broker_bus_append_unit_info(sd_bus_message *message, const Systemd1BrokerUnitInfo *info);
int systemd1_broker_bus_append_job_info(sd_bus_message *message, const Systemd1BrokerJobInfo *info);
