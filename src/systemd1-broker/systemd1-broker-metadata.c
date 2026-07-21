/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <math.h>

#include "sd-bus.h"

#include "alloc-util.h"
#include "bus-signature.h"
#include "hashmap.h"
#include "string-util.h"
#include "strv.h"

#include "systemd1-broker-metadata.h"

#define SYSTEMD1_BROKER_MAX_PROPERTIES 256U
#define SYSTEMD1_BROKER_MAX_PROPERTY_DATA (1024U * 1024U)
#define SYSTEMD1_BROKER_MAX_METADATA_DEPTH 64U

#define SYSTEMD1_UNIT_INTERFACE "org.freedesktop.systemd1.Unit"
#define SYSTEMD1_SERVICE_INTERFACE "org.freedesktop.systemd1.Service"

struct Systemd1BrokerMetadataSchema {
        Hashmap *signatures;
};

struct Systemd1BrokerProperty {
        char *interface;
        char *name;
        char *signature;
        char *value_json;
        sd_json_variant *value;
};

static bool property_name_is_reserved(const char *name) {
        return STR_IN_SET(name, "ActiveState", "Description", "Id", "Job", "LoadState", "Names", "SubState");
}

static bool interface_is_supported(const char *interface) {
        return STR_IN_SET(interface, SYSTEMD1_UNIT_INTERFACE, SYSTEMD1_SERVICE_INTERFACE);
}

static int json_variant_get_signed(sd_json_variant *value, int64_t minimum, int64_t maximum) {
        sd_json_variant_type_t type;
        int64_t signed_value;

        type = sd_json_variant_type(value);
        if (type == SD_JSON_VARIANT_INTEGER)
                signed_value = sd_json_variant_integer(value);
        else if (type == SD_JSON_VARIANT_UNSIGNED) {
                uint64_t unsigned_value = sd_json_variant_unsigned(value);

                if (unsigned_value > INT64_MAX)
                        return -ERANGE;
                signed_value = (int64_t) unsigned_value;
        } else
                return -EBADMSG;

        return signed_value < minimum || signed_value > maximum ? -ERANGE : 0;
}

static int json_variant_get_unsigned(sd_json_variant *value, uint64_t maximum) {
        sd_json_variant_type_t type;
        uint64_t unsigned_value;

        type = sd_json_variant_type(value);
        if (type == SD_JSON_VARIANT_UNSIGNED)
                unsigned_value = sd_json_variant_unsigned(value);
        else if (type == SD_JSON_VARIANT_INTEGER) {
                int64_t signed_value = sd_json_variant_integer(value);

                if (signed_value < 0)
                        return -ERANGE;
                unsigned_value = (uint64_t) signed_value;
        } else
                return -EBADMSG;

        return unsigned_value > maximum ? -ERANGE : 0;
}

static int validate_json_element(const char *signature, size_t signature_length, sd_json_variant *value, unsigned depth);

static int validate_json_array(
                const char *element_signature,
                size_t element_signature_length,
                sd_json_variant *value,
                unsigned depth) {

        if (!sd_json_variant_is_array(value))
                return -EBADMSG;

        for (size_t i = 0; i < sd_json_variant_elements(value); i++) {
                int r;

                r = validate_json_element(
                                element_signature,
                                element_signature_length,
                                sd_json_variant_by_index(value, i),
                                depth + 1);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int validate_json_dict(const char *signature, size_t signature_length, sd_json_variant *value, unsigned depth) {
        size_t key_length;
        int r;

        if (!sd_json_variant_is_array(value) || signature_length < 5)
                return -EBADMSG;

        r = signature_element_length(signature + 2, &key_length);
        if (r < 0 || key_length >= signature_length - 3)
                return -EBADMSG;

        for (size_t i = 0; i < sd_json_variant_elements(value); i++) {
                sd_json_variant *pair = sd_json_variant_by_index(value, i);

                if (!sd_json_variant_is_array(pair) || sd_json_variant_elements(pair) != 2)
                        return -EBADMSG;
                r = validate_json_element(signature + 2, key_length, sd_json_variant_by_index(pair, 0), depth + 1);
                if (r < 0)
                        return r;
                r = validate_json_element(
                                signature + 2 + key_length,
                                signature_length - 3 - key_length,
                                sd_json_variant_by_index(pair, 1),
                                depth + 1);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int validate_json_struct(const char *signature, size_t signature_length, sd_json_variant *value, unsigned depth) {
        const char *p, *end;
        size_t index = 0;

        if (!sd_json_variant_is_array(value) || signature_length < 3)
                return -EBADMSG;

        p = signature + 1;
        end = signature + signature_length - 1;
        while (p < end) {
                size_t element_length;
                int r;

                if (index >= sd_json_variant_elements(value))
                        return -EBADMSG;
                r = signature_element_length(p, &element_length);
                if (r < 0 || p + element_length > end)
                        return -EBADMSG;
                r = validate_json_element(p, element_length, sd_json_variant_by_index(value, index), depth + 1);
                if (r < 0)
                        return r;
                p += element_length;
                index++;
        }

        return index == sd_json_variant_elements(value) ? 0 : -EBADMSG;
}

static int validate_json_variant(sd_json_variant *value, unsigned depth) {
        sd_json_variant *signature_value, *nested_value;
        const char *signature;
        size_t signature_length;

        if (!sd_json_variant_is_object(value) || sd_json_variant_elements(value) != 4)
                return -EBADMSG;

        signature_value = sd_json_variant_by_key(value, "signature");
        nested_value = sd_json_variant_by_key(value, "value");
        if (!sd_json_variant_is_string(signature_value) || !nested_value)
                return -EBADMSG;
        signature = sd_json_variant_string(signature_value);
        if (!signature_is_single(signature, false))
                return -EBADMSG;
        if (strchr(signature, 'h'))
                return -EOPNOTSUPP;

        signature_length = strlen(signature);
        return validate_json_element(signature, signature_length, nested_value, depth + 1);
}

static int validate_json_element(const char *signature, size_t signature_length, sd_json_variant *value, unsigned depth) {
        const char *string;

        if (depth > SYSTEMD1_BROKER_MAX_METADATA_DEPTH || !value || sd_json_variant_is_null(value))
                return -EBADMSG;

        switch (signature[0]) {
        case 'y':
                return signature_length == 1 ? json_variant_get_unsigned(value, UINT8_MAX) : -EBADMSG;
        case 'b':
                return signature_length == 1 && sd_json_variant_is_boolean(value) ? 0 : -EBADMSG;
        case 'n':
                return signature_length == 1 ? json_variant_get_signed(value, INT16_MIN, INT16_MAX) : -EBADMSG;
        case 'q':
                return signature_length == 1 ? json_variant_get_unsigned(value, UINT16_MAX) : -EBADMSG;
        case 'i':
                return signature_length == 1 ? json_variant_get_signed(value, INT32_MIN, INT32_MAX) : -EBADMSG;
        case 'u':
                return signature_length == 1 ? json_variant_get_unsigned(value, UINT32_MAX) : -EBADMSG;
        case 'x':
                return signature_length == 1 ? json_variant_get_signed(value, INT64_MIN, INT64_MAX) : -EBADMSG;
        case 't':
                return signature_length == 1 ? json_variant_get_unsigned(value, UINT64_MAX) : -EBADMSG;
        case 'd':
                if (signature_length != 1 || !sd_json_variant_is_number(value))
                        return -EBADMSG;
                return isfinite(sd_json_variant_real(value)) ? 0 : -ERANGE;
        case 's':
                return signature_length == 1 && sd_json_variant_is_string(value) ? 0 : -EBADMSG;
        case 'o':
                if (signature_length != 1 || !sd_json_variant_is_string(value))
                        return -EBADMSG;
                return sd_bus_object_path_is_valid(sd_json_variant_string(value)) > 0 ? 0 : -EBADMSG;
        case 'g':
                if (signature_length != 1 || !sd_json_variant_is_string(value))
                        return -EBADMSG;
                string = sd_json_variant_string(value);
                return signature_is_valid(string, false) ? 0 : -EBADMSG;
        case 'h':
                return -EOPNOTSUPP;
        case 'a':
                if (signature_length < 2)
                        return -EBADMSG;
                if (signature[1] == '{')
                        return validate_json_dict(signature, signature_length, value, depth);
                return validate_json_array(signature + 1, signature_length - 1, value, depth);
        case '(':
                return validate_json_struct(signature, signature_length, value, depth);
        case 'v':
                return signature_length == 1 ? validate_json_variant(value, depth) : -EBADMSG;
        default:
                return -EBADMSG;
        }
}

static Systemd1BrokerMetadataSchema* metadata_schema_new(void) {
        return new0(Systemd1BrokerMetadataSchema, 1);
}

Systemd1BrokerMetadataSchema* systemd1_broker_metadata_schema_free(Systemd1BrokerMetadataSchema *schema) {
        if (!schema)
                return NULL;

        hashmap_free(schema->signatures);
        return mfree(schema);
}

DEFINE_TRIVIAL_CLEANUP_FUNC(Systemd1BrokerMetadataSchema*, systemd1_broker_metadata_schema_free);

static int metadata_schema_clone(Systemd1BrokerMetadataSchema *current, Systemd1BrokerMetadataSchema **ret) {
        _cleanup_(systemd1_broker_metadata_schema_freep) Systemd1BrokerMetadataSchema *schema = NULL;
        const char *key, *signature;
        int r;

        schema = metadata_schema_new();
        if (!schema)
                return -ENOMEM;

        HASHMAP_FOREACH_KEY(signature, key, current ? current->signatures : NULL) {
                r = hashmap_put_strdup(&schema->signatures, key, signature);
                if (r < 0)
                        return r;
        }

        *ret = TAKE_PTR(schema);
        return 0;
}

void systemd1_broker_properties_free(Systemd1BrokerProperty *properties, size_t n_properties) {
        if (!properties)
                return;

        for (size_t i = 0; i < n_properties; i++) {
                free(properties[i].interface);
                free(properties[i].name);
                free(properties[i].signature);
                free(properties[i].value_json);
                sd_json_variant_unref(properties[i].value);
        }
        free(properties);
}

static int property_data_size_add(size_t *size, const char *value) {
        size_t n;

        if (!value)
                return -EBADMSG;
        n = strlen(value);
        if (n > SYSTEMD1_BROKER_MAX_PROPERTY_DATA - *size)
                return -E2BIG;
        *size += n;
        return 0;
}

static int property_prepare(const Systemd1BrokerBackendProperty *source, Systemd1BrokerProperty *ret) {
        _cleanup_(sd_json_variant_unrefp) sd_json_variant *value = NULL;
        _cleanup_free_ char *canonical = NULL;
        int r;

        r = sd_json_parse(source->value_json, 0, &value, NULL, NULL);
        if (r < 0)
                return -EBADMSG;
        r = validate_json_element(source->signature, strlen(source->signature), value, 0);
        if (r < 0)
                return r;
        r = sd_json_variant_format(value, 0, &canonical);
        if (r < 0)
                return r;

        *ret = (Systemd1BrokerProperty) {
                .interface = strdup(source->interface),
                .name = strdup(source->name),
                .signature = strdup(source->signature),
                .value_json = TAKE_PTR(canonical),
                .value = TAKE_PTR(value),
        };
        if (!ret->interface || !ret->name || !ret->signature)
                return -ENOMEM;

        return 0;
}

static bool property_key_equal(const Systemd1BrokerProperty *a, const Systemd1BrokerProperty *b) {
        return streq(a->interface, b->interface) && streq(a->name, b->name);
}

int systemd1_broker_metadata_prepare(
                Systemd1BrokerMetadataSchema *current_schema,
                const Systemd1BrokerBackendUnitSnapshot *snapshot,
                Systemd1BrokerProperty **ret_properties,
                size_t *ret_n_properties,
                Systemd1BrokerMetadataSchema **ret_schema) {

        _cleanup_(systemd1_broker_metadata_schema_freep) Systemd1BrokerMetadataSchema *schema = NULL;
        Systemd1BrokerProperty *properties = NULL;
        size_t data_size = 0, n_properties = 0;
        int r;

        assert(snapshot);
        assert(ret_properties);
        assert(ret_n_properties);
        assert(ret_schema);

        if (snapshot->size < sizeof(Systemd1BrokerBackendUnitSnapshot) ||
            snapshot->n_properties > SYSTEMD1_BROKER_MAX_PROPERTIES ||
            (snapshot->n_properties > 0 && !snapshot->properties))
                return snapshot->n_properties > SYSTEMD1_BROKER_MAX_PROPERTIES ? -E2BIG : -EBADMSG;

        for (size_t i = 0; i < snapshot->n_properties; i++) {
                const Systemd1BrokerBackendProperty *source = snapshot->properties + i;

                if (source->size < sizeof(Systemd1BrokerBackendProperty))
                        return -EBADMSG;
                r = property_data_size_add(&data_size, source->interface);
                if (r < 0)
                        return r;
                r = property_data_size_add(&data_size, source->name);
                if (r < 0)
                        return r;
                r = property_data_size_add(&data_size, source->signature);
                if (r < 0)
                        return r;
                r = property_data_size_add(&data_size, source->value_json);
                if (r < 0)
                        return r;
        }

        r = metadata_schema_clone(current_schema, &schema);
        if (r < 0)
                return r;
        if (snapshot->n_properties > 0) {
                properties = new0(Systemd1BrokerProperty, snapshot->n_properties);
                if (!properties)
                        return -ENOMEM;
        }

        for (size_t i = 0; i < snapshot->n_properties; i++) {
                const Systemd1BrokerBackendProperty *source = snapshot->properties + i;
                _cleanup_free_ char *schema_key = NULL;
                const char *registered_signature;

                if (sd_bus_interface_name_is_valid(source->interface) <= 0 ||
                    sd_bus_member_name_is_valid(source->name) <= 0 ||
                    !signature_is_single(source->signature, false)) {
                        r = -EBADMSG;
                        goto fail;
                }
                if (!interface_is_supported(source->interface) ||
                    property_name_is_reserved(source->name) ||
                    strchr(source->signature, 'h'))
                        continue;

                r = property_prepare(source, properties + n_properties);
                if (r == -EOPNOTSUPP)
                        continue;
                if (r < 0)
                        goto fail;

                for (size_t j = 0; j < n_properties; j++)
                        if (property_key_equal(properties + j, properties + n_properties) ||
                            streq(properties[j].name, properties[n_properties].name)) {
                                r = -EEXIST;
                                goto fail;
                        }

                schema_key = strjoin(source->interface, "\n", source->name);
                if (!schema_key) {
                        r = -ENOMEM;
                        goto fail;
                }
                registered_signature = hashmap_get(schema->signatures, schema_key);
                if (registered_signature && !streq(registered_signature, source->signature)) {
                        r = -EBADMSG;
                        goto fail;
                }
                if (!registered_signature) {
                        r = hashmap_put_strdup(&schema->signatures, schema_key, source->signature);
                        if (r < 0)
                                goto fail;
                }
                n_properties++;
        }

        *ret_properties = properties;
        *ret_n_properties = n_properties;
        *ret_schema = TAKE_PTR(schema);
        return 0;

fail:
        systemd1_broker_properties_free(properties, n_properties + 1);
        return r;
}

const Systemd1BrokerProperty* systemd1_broker_properties_find(
                const Systemd1BrokerProperty *properties,
                size_t n_properties,
                const char *interface,
                const char *name) {

        for (size_t i = 0; i < n_properties; i++)
                if (streq(properties[i].interface, interface) && streq(properties[i].name, name))
                        return properties + i;
        return NULL;
}

const Systemd1BrokerProperty* systemd1_broker_properties_at(
                const Systemd1BrokerProperty *properties,
                size_t n_properties,
                size_t index) {

        return index < n_properties ? properties + index : NULL;
}

bool systemd1_broker_properties_equal(
                const Systemd1BrokerProperty *a,
                size_t n_a,
                const Systemd1BrokerProperty *b,
                size_t n_b) {

        if (n_a != n_b)
                return false;
        for (size_t i = 0; i < n_a; i++) {
                const Systemd1BrokerProperty *other;

                other = systemd1_broker_properties_find(b, n_b, a[i].interface, a[i].name);
                if (!other || !streq(a[i].signature, other->signature) || !streq(a[i].value_json, other->value_json))
                        return false;
        }
        return true;
}

const char* systemd1_broker_property_interface(const Systemd1BrokerProperty *property) {
        return ASSERT_PTR(property)->interface;
}

const char* systemd1_broker_property_name(const Systemd1BrokerProperty *property) {
        return ASSERT_PTR(property)->name;
}

const char* systemd1_broker_property_signature(const Systemd1BrokerProperty *property) {
        return ASSERT_PTR(property)->signature;
}

const char* systemd1_broker_property_value_json(const Systemd1BrokerProperty *property) {
        return ASSERT_PTR(property)->value_json;
}

sd_json_variant* systemd1_broker_property_value(const Systemd1BrokerProperty *property) {
        return ASSERT_PTR(property)->value;
}

static int64_t json_variant_signed_value(sd_json_variant *value) {
        return sd_json_variant_type(value) == SD_JSON_VARIANT_INTEGER ?
                sd_json_variant_integer(value) : (int64_t) sd_json_variant_unsigned(value);
}

static uint64_t json_variant_unsigned_value(sd_json_variant *value) {
        return sd_json_variant_type(value) == SD_JSON_VARIANT_UNSIGNED ?
                sd_json_variant_unsigned(value) : (uint64_t) sd_json_variant_integer(value);
}

static int append_json_element(
                sd_bus_message *message,
                const char *signature,
                size_t signature_length,
                sd_json_variant *value,
                unsigned depth);

static int append_json_array(
                sd_bus_message *message,
                const char *signature,
                size_t signature_length,
                sd_json_variant *value,
                unsigned depth) {

        _cleanup_free_ char *element_signature = NULL;
        int r;

        element_signature = strndup(signature + 1, signature_length - 1);
        if (!element_signature)
                return -ENOMEM;
        r = sd_bus_message_open_container(message, 'a', element_signature);
        if (r < 0)
                return r;
        for (size_t i = 0; i < sd_json_variant_elements(value); i++) {
                r = append_json_element(
                                message,
                                signature + 1,
                                signature_length - 1,
                                sd_json_variant_by_index(value, i),
                                depth + 1);
                if (r < 0)
                        return r;
        }
        return sd_bus_message_close_container(message);
}

static int append_json_dict(
                sd_bus_message *message,
                const char *signature,
                size_t signature_length,
                sd_json_variant *value,
                unsigned depth) {

        _cleanup_free_ char *array_signature = NULL, *entry_signature = NULL;
        size_t key_length;
        int r;

        r = signature_element_length(signature + 2, &key_length);
        if (r < 0)
                return r;
        array_signature = strndup(signature + 1, signature_length - 1);
        entry_signature = strndup(signature + 2, signature_length - 3);
        if (!array_signature || !entry_signature)
                return -ENOMEM;

        r = sd_bus_message_open_container(message, 'a', array_signature);
        if (r < 0)
                return r;
        for (size_t i = 0; i < sd_json_variant_elements(value); i++) {
                sd_json_variant *pair = sd_json_variant_by_index(value, i);

                r = sd_bus_message_open_container(message, 'e', entry_signature);
                if (r < 0)
                        return r;
                r = append_json_element(message, signature + 2, key_length, sd_json_variant_by_index(pair, 0), depth + 1);
                if (r < 0)
                        return r;
                r = append_json_element(
                                message,
                                signature + 2 + key_length,
                                signature_length - 3 - key_length,
                                sd_json_variant_by_index(pair, 1),
                                depth + 1);
                if (r < 0)
                        return r;
                r = sd_bus_message_close_container(message);
                if (r < 0)
                        return r;
        }
        return sd_bus_message_close_container(message);
}

static int append_json_struct(
                sd_bus_message *message,
                const char *signature,
                size_t signature_length,
                sd_json_variant *value,
                unsigned depth) {

        _cleanup_free_ char *contents = NULL;
        const char *p, *end;
        size_t index = 0;
        int r;

        contents = strndup(signature + 1, signature_length - 2);
        if (!contents)
                return -ENOMEM;
        r = sd_bus_message_open_container(message, 'r', contents);
        if (r < 0)
                return r;

        p = signature + 1;
        end = signature + signature_length - 1;
        while (p < end) {
                size_t element_length;

                r = signature_element_length(p, &element_length);
                if (r < 0)
                        return r;
                r = append_json_element(message, p, element_length, sd_json_variant_by_index(value, index), depth + 1);
                if (r < 0)
                        return r;
                p += element_length;
                index++;
        }
        return sd_bus_message_close_container(message);
}

static int append_json_variant(sd_bus_message *message, sd_json_variant *value, unsigned depth) {
        sd_json_variant *signature_value, *nested_value;
        const char *signature;
        int r;

        signature_value = sd_json_variant_by_key(value, "signature");
        nested_value = sd_json_variant_by_key(value, "value");
        signature = sd_json_variant_string(signature_value);
        r = sd_bus_message_open_container(message, 'v', signature);
        if (r < 0)
                return r;
        r = append_json_element(message, signature, strlen(signature), nested_value, depth + 1);
        if (r < 0)
                return r;
        return sd_bus_message_close_container(message);
}

static int append_json_element(
                sd_bus_message *message,
                const char *signature,
                size_t signature_length,
                sd_json_variant *value,
                unsigned depth) {

        if (depth > SYSTEMD1_BROKER_MAX_METADATA_DEPTH)
                return -E2BIG;

        switch (signature[0]) {
        case 'y': {
                uint8_t converted = (uint8_t) json_variant_unsigned_value(value);
                return sd_bus_message_append_basic(message, 'y', &converted);
        }
        case 'b': {
                int converted = sd_json_variant_boolean(value);
                return sd_bus_message_append_basic(message, 'b', &converted);
        }
        case 'n': {
                int16_t converted = (int16_t) json_variant_signed_value(value);
                return sd_bus_message_append_basic(message, 'n', &converted);
        }
        case 'q': {
                uint16_t converted = (uint16_t) json_variant_unsigned_value(value);
                return sd_bus_message_append_basic(message, 'q', &converted);
        }
        case 'i': {
                int32_t converted = (int32_t) json_variant_signed_value(value);
                return sd_bus_message_append_basic(message, 'i', &converted);
        }
        case 'u': {
                uint32_t converted = (uint32_t) json_variant_unsigned_value(value);
                return sd_bus_message_append_basic(message, 'u', &converted);
        }
        case 'x': {
                int64_t converted = json_variant_signed_value(value);
                return sd_bus_message_append_basic(message, 'x', &converted);
        }
        case 't': {
                uint64_t converted = json_variant_unsigned_value(value);
                return sd_bus_message_append_basic(message, 't', &converted);
        }
        case 'd': {
                double converted = sd_json_variant_real(value);
                return sd_bus_message_append_basic(message, 'd', &converted);
        }
        case 's':
        case 'o':
        case 'g':
                return sd_bus_message_append_basic(message, signature[0], sd_json_variant_string(value));
        case 'a':
                if (signature[1] == '{')
                        return append_json_dict(message, signature, signature_length, value, depth);
                return append_json_array(message, signature, signature_length, value, depth);
        case '(':
                return append_json_struct(message, signature, signature_length, value, depth);
        case 'v':
                return append_json_variant(message, value, depth);
        default:
                return -EOPNOTSUPP;
        }
}

int systemd1_broker_property_append_value(sd_bus_message *message, const Systemd1BrokerProperty *property) {
        assert(message);
        assert(property);

        return append_json_element(message, property->signature, strlen(property->signature), property->value, 0);
}
