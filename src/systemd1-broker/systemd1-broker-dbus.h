/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "sd-bus.h"

#include "systemd1-broker.h"

int systemd1_broker_dbus_add_manager(sd_bus *bus, Systemd1BrokerManager *manager);
int systemd1_broker_dbus_emit_job_new(sd_bus *bus, const Systemd1BrokerJobInfo *info);
int systemd1_broker_dbus_emit_job_removed(sd_bus *bus, const Systemd1BrokerJobInfo *info, const char *result);
int systemd1_broker_dbus_emit_unit_properties_changed(sd_bus *bus, Systemd1BrokerUnit *unit);
int systemd1_broker_dbus_complete_jobs(sd_bus *bus, Systemd1BrokerManager *manager);
int systemd1_broker_serve_bus_fd_full(int fd, Systemd1BrokerManager *manager, bool add_test_api, bool *ret_quit);
int systemd1_broker_serve_bus_fd(int fd, Systemd1BrokerManager *manager, bool *ret_quit);
