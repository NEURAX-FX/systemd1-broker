/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "sd-bus.h"
#include "sd-json.h"

#include "systemd1-broker.h"

typedef struct Systemd1BrokerMetadataSchema Systemd1BrokerMetadataSchema;
typedef struct Systemd1BrokerProperty Systemd1BrokerProperty;

Systemd1BrokerMetadataSchema* systemd1_broker_metadata_schema_free(Systemd1BrokerMetadataSchema *schema);
int systemd1_broker_metadata_prepare(
                Systemd1BrokerMetadataSchema *current_schema,
                const Systemd1BrokerBackendUnitSnapshot *snapshot,
                Systemd1BrokerProperty **ret_properties,
                size_t *ret_n_properties,
                Systemd1BrokerMetadataSchema **ret_schema);

void systemd1_broker_properties_free(Systemd1BrokerProperty *properties, size_t n_properties);
bool systemd1_broker_properties_equal(
                const Systemd1BrokerProperty *a,
                size_t n_a,
                const Systemd1BrokerProperty *b,
                size_t n_b);
const Systemd1BrokerProperty* systemd1_broker_properties_at(
                const Systemd1BrokerProperty *properties,
                size_t n_properties,
                size_t index);
const Systemd1BrokerProperty* systemd1_broker_properties_find(
                const Systemd1BrokerProperty *properties,
                size_t n_properties,
                const char *interface,
                const char *name);

sd_json_variant* systemd1_broker_property_value(const Systemd1BrokerProperty *property);
int systemd1_broker_property_append_value(sd_bus_message *message, const Systemd1BrokerProperty *property);
