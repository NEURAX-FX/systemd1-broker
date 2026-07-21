# systemd1 Broker Dynamic Metadata Design

## Purpose

Make the systemd1 broker expose the standard systemd metadata that a backend can
actually provide, without synthesizing misleading empty strings, zero PIDs, or
other placeholder values in `systemctl show` and `systemctl status`.

The backend publishes a typed property snapshot. The broker validates that
snapshot and translates it into the standard
`org.freedesktop.systemd1.Unit` and `org.freedesktop.systemd1.Service` D-Bus
interfaces. Adding another standard property to a backend must not require a
matching broker source change unless that property conflicts with broker-owned
state.

## Goals

1. Let `systemctl show`, `systemctl status`, and `systemctl cat` consume real
   backend metadata such as `FragmentPath`, `MainPID`, service type, execution
   configuration, and dependencies.
2. Omit optional properties from generic property dumps when the backend did not
   provide them.
3. Refresh one complete per-unit snapshot for each D-Bus property query.
4. Keep the last valid per-unit snapshot when a later backend refresh fails.
5. Preserve one internally consistent generation of metadata throughout a
   single D-Bus reply.
6. Permit typed passthrough of future standard Unit and Service properties
   without adding one C structure member per property.
7. Keep the servicectl backend shared library independent of libsystemd.

## Non-Goals

1. Emulating every property exported by systemd PID 1.
2. Publishing servicectl-specific names such as `managed_by`, `dinit_name`, or
   `logger_name` on a custom D-Bus interface in this change.
3. Allowing a backend to add arbitrary D-Bus interfaces or override the broker's
   unit identity, job model, or state mapping.
4. Adding an event-stream subscription between the broker and servicectl.
5. Making metadata refresh repair, start, stop, or otherwise mutate a service.
6. Returning fabricated unit-file paths or process identifiers when no reliable
   source exists.

## Existing Constraints

The current catalog ABI carries only `id`, `description`, and an init-neutral
state. The broker consequently returns empty or zero values for every other
property in its static Unit and Service vtables. That behavior explains both of
the observed shortcomings:

1. `systemctl show` prints `MainPID=0`, `ExecMainPID=0`, and other placeholders.
2. `systemctl cat` reports no files because `FragmentPath` is always empty.

Servicectl already has authoritative data for several missing values. Its unit
snapshot includes `source_path`, `main_pid`, runtime status, state, and related
identity fields. Parsing a unit file provides service type, execution settings,
and dependencies. The missing layer is a generic typed metadata contract from
the backend to the broker.

## Architecture

The metadata path has four layers:

1. The servicectl control API builds one rich snapshot from unit-file metadata
   and current runtime observation.
2. The servicectl C backend converts that response into an ABI-owned typed
   property snapshot.
3. The broker validates and atomically caches the snapshot on its unit object.
4. A broker-owned D-Bus Properties dispatcher emits core broker properties plus
   only the optional backend properties present in that cached generation.

The backend chooses which standard properties it can support. The broker owns
the D-Bus protocol, validation, cache lifetime, and compatibility behavior.

## Backend ABI

The ABI gains a generic property record and a complete per-unit snapshot:

```c
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
```

Two operations are appended to `Systemd1BrokerBackendOps`:

```c
int (*get_unit_snapshot)(
                void *userdata,
                const char *unit_name,
                const Systemd1BrokerBackendUnitExtra *extra,
                Systemd1BrokerBackendUnitSnapshot **ret_snapshot);

void (*free_unit_snapshot)(
                void *userdata,
                Systemd1BrokerBackendUnitSnapshot *snapshot);
```

`get_unit_snapshot` performs one read-only backend query. A successful result is
one complete generation. The backend owns the returned structure, property
array, and all reachable strings until `free_unit_snapshot` is called exactly
once. The broker copies all accepted data before releasing it.

The snapshot state updates the broker's init-neutral state at the same time as
the metadata. This prevents `ActiveState` from one observation being combined
with `MainPID` from another observation. The optional description refreshes the
catalog description when non-empty.

The existing `status` operation remains available for operation completion and
callers that need state only. Property queries use `get_unit_snapshot` and do not
perform a second `status` call.

The broker and its shipped backend are versioned and deployed together. The new
snapshot operations are required by this ABI revision; no compatibility shim is
added for an older unshipped backend module.

## Typed Property Encoding

Each property identifies one standard interface, one D-Bus member name, one
complete D-Bus signature, and a JSON-encoded value. JSON keeps the C plugin ABI
independent of libsystemd while permitting nested D-Bus values.

The encoding rules are:

1. `s`, `o`, and `g` use a JSON string. Object paths and signatures receive their
   normal D-Bus validation.
2. `b` uses a JSON boolean.
3. Integer D-Bus types use a JSON integer and must fit their exact signed or
   unsigned range.
4. `d` uses a finite JSON number.
5. Arrays use JSON arrays.
6. Structs use positional JSON arrays with exactly the required number of
   elements.
7. Dictionary arrays use JSON arrays of two-element key/value arrays so that
   non-string D-Bus keys remain representable.
8. Variants use `{"signature":"s","value":"..."}` with the value encoded
   according to the nested signature.
9. UNIX file descriptors (`h`) are rejected because their lifetime cannot be
   represented safely across this ABI.
10. JSON null is not a value for any supported D-Bus signature.

The broker parses `value_json` with sd-json and recursively appends the value to
an sd-bus message according to `signature`. It does not infer a signature from
JSON types.

## Property Validation

The broker accepts properties only on:

1. `org.freedesktop.systemd1.Unit`;
2. `org.freedesktop.systemd1.Service`.

Interface names, property names, and signatures must be syntactically valid.
The signature must contain exactly one complete type. Property keys formed from
interface and name must be unique within a snapshot.

The following values remain broker-owned and cannot be overridden by a backend:

1. `Unit.Id` and `Unit.Names`;
2. `Unit.Description`;
3. `Unit.LoadState`;
4. `Unit.ActiveState` and `Unit.SubState`;
5. `Unit.Job`;
6. methods and signals on either interface.

These are the only properties that the broker always emits. Values such as
`Following`, `NeedDaemonReload`, and `InvocationID` are optional systemd
metadata, not broker-owned facts, and are omitted unless a backend provides a
valid typed value.

The broker does not maintain a property-name whitelist beyond these interface
and reserved-name rules. A trusted backend may therefore publish a newly added
standard property without a broker rebuild. Standard clients ignore names they
do not understand.

The manager maintains a schema registry keyed by interface and property name.
The first accepted occurrence establishes the D-Bus signature for that key.
Every later occurrence, on any unit or generation, must use the same signature.
The registry retains established keys until broker shutdown even if no current
unit provides a value. This preserves the D-Bus rule that one property on one
interface has a stable type. A snapshot that conflicts with an established
signature is rejected as a whole. Registering new schema keys and replacing the
unit snapshot are prepared and committed atomically.

Because `GetAll("")` combines Unit and Service properties into one `a{sv}` whose
keys contain no interface qualifier, accepted dynamic properties must also have
unique member names across both interfaces. A property name that collides with a
broker-owned core name is treated as a reserved override and dropped. Two
accepted dynamic properties with the same member name but different interfaces
reject the snapshot as a whole.

An unsupported interface, reserved override, invalid member name, or unsupported
signature is dropped with a rate-limited diagnostic. Duplicate keys, malformed
JSON, type mismatches, or out-of-range values reject the new snapshot as a
whole, leaving the previous valid cache intact. This avoids publishing a partial
generation.

One snapshot is limited to 256 properties and 1 MiB of encoded property data.
These limits include all property names, signatures, and JSON values.

## Presence Semantics

Presence is represented by inclusion in the property array. JSON zero, false,
or an empty collection is a real provided value; absence is not inferred from
the value.

The servicectl backend omits a property when it cannot establish a reliable
value. In particular, it does not publish `MainPID=0`, an empty
`FragmentPath`, or guessed execution metadata.

For a generic `Properties.GetAll` request, the broker emits:

1. its seven required core properties;
2. the optional backend properties present in the current cached snapshot.

Missing optional properties are not included. This is what makes default
`systemctl show` output reflect backend capability instead of a static list of
placeholders.

Explicit property reads require a narrow compatibility exception. `systemctl
cat` explicitly reads `FragmentPath` and `DropInPaths`; returning
`UnknownProperty` would turn a missing fragment into a D-Bus failure. For these
standard client probes, the broker returns the type-correct neutral value when
the property is absent:

1. `FragmentPath` returns `""`;
2. `DropInPaths` returns an empty string array.

These neutral values are not included in `GetAll`. Other absent optional
properties return `org.freedesktop.DBus.Error.UnknownProperty` when explicitly
requested.

## Refresh And Cache Semantics

Every D-Bus Properties request for a unit starts at most one backend snapshot
query. `GetAll("")`, which is used by systemctl, refreshes once and uses that one
generation while emitting properties from both Unit and Service interfaces.
Individual property getters also refresh once per D-Bus request.

On a successful refresh the broker:

1. validates and fully copies the new snapshot before mutation;
2. updates description and backend state;
3. atomically replaces the old typed property set;
4. increments a per-unit metadata generation;
5. releases the backend-owned response.

On a failed refresh the broker logs the failure and serves the most recent valid
snapshot. If no valid rich snapshot has ever been received, it serves only its
core properties and the explicit neutral-value compatibility probes described
above.

A successful snapshot removes properties omitted by the backend in that new
generation. A failed refresh never removes cached properties. Cache lifetime is
the lifetime of the broker unit, and catalog removal releases all cached values.

The broker is currently single-threaded, but preparation-before-replacement is
still required so allocation or validation failure cannot leave a partially
updated unit.

## D-Bus Dispatch

The static fallback vtables continue to define Unit and Service methods and the
fixed core properties. A fallback object callback handles the standard
`org.freedesktop.DBus.Properties` methods for unit paths before the generic
vtable dispatcher:

1. `GetAll` refreshes once, then serializes the requested interface or both
   standard interfaces when the interface argument is empty.
2. `Get` refreshes once, then returns a core property, a present dynamic
   property, one of the explicit compatibility neutral values, or
   `UnknownProperty`.
3. `Set` remains unsupported for read-only metadata.

The same callback handles introspection for unit paths by combining the fixed
method schema with property definitions present in the unit's last valid typed
snapshot. Signatures come from the manager-wide schema registry, so the same key
never changes type between objects. When refresh fails before any valid snapshot
exists, introspection contains only the fixed core surface.

After a successful replacement, the broker compares property keys, signatures,
and canonical JSON values. It emits invalidating `PropertiesChanged` signals for
added, changed, and removed dynamic properties. Clients that care about changes
then fetch the current value. Existing broker state and job signals remain the
authority for `ActiveState`, `SubState`, and `Job`.

## Servicectl Control API

The servicectl control plane adds a per-unit endpoint on its existing Unix
socket:

```text
GET /v1/units/<escaped-unit-name>
```

The response contains one existing `UnitSnapshot` plus a typed property array:

```json
{
  "unit": {
    "name": "example",
    "description": "Example service",
    "state": "STARTED",
    "source_path": "/etc/systemd/system/example.service",
    "main_pid": "4242"
  },
  "systemd_properties": [
    {
      "interface": "org.freedesktop.systemd1.Unit",
      "name": "FragmentPath",
      "signature": "s",
      "value": "/etc/systemd/system/example.service"
    },
    {
      "interface": "org.freedesktop.systemd1.Service",
      "name": "MainPID",
      "signature": "u",
      "value": 4242
    }
  ]
}
```

The endpoint normalizes one optional `.service` suffix and rejects other unit
types in this implementation. It parses the unit, collects runtime state, and
queries the cgroup tracker at most once each per request. Existing `/v1/units`
catalog behavior remains unchanged so catalog enumeration does not carry large
execution-property payloads.

The servicectl `source_path` field names the regular unit fragment. It maps to
systemd's `FragmentPath`. It does not map to systemd's `SourcePath`, whose
meaning is the origin from which a unit was generated or converted. The backend
publishes `SourcePath` only if servicectl later exposes that distinct concept.

The cgroup tracker is the authoritative source for `Unit.ControlGroup`.
Servicectl converts a tracked filesystem path such as
`/sys/fs/cgroup/servicectl.slice/system/example` to systemd's hierarchy-relative
form `/servicectl.slice/system/example`. It publishes only clean absolute paths
inside the cgroup v2 root. An unavailable tracker, an untracked unit, or an
out-of-root path omits the optional property instead of deriving or fabricating
one from the unit name or PID.

The initial servicectl property set is:

1. Unit: `FragmentPath`, `ControlGroup`, `UnitFileState`, `Requires`, `Wants`,
   `Before`, and `After` when each has a reliable source.
2. Service: `MainPID`, `ExecMainPID`, `StatusText`, `Result`, `Type`,
   `ExecStart`, `WorkingDirectory`, `User`, and `Group` when each has a reliable
   source and can be encoded with the standard systemd signature.

An empty string from a parser is not automatically published. Values whose
systemd semantics differ from the servicectl concept are also omitted. For
example, a wrapper process is not exposed as `MainPID` when servicectl has a
different observed application PID.

The C backend forwards the typed properties through the ABI after validating
the HTTP response structure. It continues to use libc only. It does not construct
sd-bus messages or link against libsystemd.

## Failure Handling

1. A per-unit HTTP 404 maps to backend state `ABSENT` and an empty optional
   property set.
2. Transport errors, non-success responses, malformed response JSON, and typed
   value errors do not replace the last valid broker cache.
3. A successful response with no optional properties deliberately clears the
   previous optional property set.
4. A malformed single property rejects the complete generation rather than
   mixing old and new values.
5. Backend result memory is freed on every success and failure path after the
   backend transfers a response to the broker.
6. Diagnostics identify the unit and failed contract layer without logging
   arbitrary property payloads that could contain sensitive execution data.

## Testing

Servicectl tests verify:

1. the per-unit endpoint returns one normalized unit;
2. real source path, cgroup path, and PID values become typed properties;
3. unavailable values are omitted rather than encoded as empty or zero;
4. cgroup, dependency, and service execution fields use their standard
   signatures and hierarchy semantics;
5. invalid names and missing units return the expected HTTP status;
6. `/v1/units` catalog responses remain unchanged.

Backend tests verify:

1. the backend requests the per-unit endpoint exactly once;
2. scalar, array, and structured values cross the ABI with exact JSON;
3. state and metadata come from the same response;
4. ownership cleanup frees every nested allocation;
5. malformed JSON, oversized responses, and HTTP errors return negative
   errno-style results;
6. the shared library remains linked only to libc and exports only its ABI entry
   point.

Broker tests verify:

1. `GetAll("")` invokes `get_unit_snapshot` once and emits core plus present
   Unit and Service properties;
2. absent optional values do not appear in `GetAll`;
3. explicit absent `FragmentPath` and `DropInPaths` return neutral values;
4. other explicit absent properties return `UnknownProperty`;
5. each supported JSON shape is appended with its exact D-Bus signature;
6. malformed, duplicate, reserved, oversized, and out-of-range properties are
   handled according to the validation policy;
7. a failed refresh serves the previous complete generation;
8. a successful smaller snapshot removes omitted properties;
9. state, description, and optional metadata update atomically;
10. introspection reflects the current dynamic property schema;
11. changed and removed properties emit invalidation signals.

Live validation uses a real servicectl-managed unit and checks:

```sh
systemctl show example.service
systemctl status example.service
systemctl cat example.service
systemctl show example.service -p FragmentPath -p MainPID -p Type
busctl introspect org.freedesktop.systemd1 \
  /org/freedesktop/systemd1/unit/example_2eservice
```

The output must contain real values for properties available from servicectl and
must not contain placeholder values for optional properties that servicectl did
not provide.

## Implementation Order

1. Add failing servicectl endpoint and property-encoding tests.
2. Implement the per-unit typed property response in servicectl.
3. Extend the backend ABI and fake backend test support.
4. Add failing C backend decoding and ownership tests.
5. Implement servicectl backend snapshot decoding.
6. Add broker property snapshot validation, typed JSON decoding, and atomic
   cache replacement tests.
7. Implement the broker snapshot cache and generic D-Bus Properties dispatcher.
8. Add dynamic introspection and property invalidation tests.
9. Run focused suites, linkage checks, and live systemctl validation.
