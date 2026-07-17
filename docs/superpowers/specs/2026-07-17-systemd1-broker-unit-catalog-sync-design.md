# systemd1 Broker Unit Catalog Synchronization Design

## Purpose

Make the systemd1 broker's unit catalog reflect every service known to
`servicectl`, so `systemctl list-units` and `systemctl list-units --all` operate
on the same application-service set as `servicectl list`.

This extends the existing backend boundary. The broker remains responsible for
systemd-compatible filtering and D-Bus objects; the backend remains responsible
for returning init-neutral unit identities and states.

## Goals

1. Synchronize discovered, enabled, and runtime-known servicectl units into the
   broker catalog.
2. Preserve the existing `/v1/units` behavior for current servicectl API users.
3. Use a structured local API rather than parsing CLI output.
4. Keep the servicectl backend shared library independent of libsystemd.
5. Keep the last complete catalog when a later synchronization attempt fails.
6. Make every Manager `ListUnits*()` method use one synchronized catalog.

## Non-Goals

1. Exposing servicectl's internal orchestration helpers as systemd units.
2. Adding dependency metadata or unit-file install state in this change.
3. Adding a long-lived watch connection between the broker and servicectl.
4. Replacing per-unit `Status`, `Start`, or `Stop` operations.
5. Making the broker parse servicectl CLI output.

## Source Of Truth

The servicectl control-plane API is the catalog source of truth.

`GET /v1/units` retains its existing behavior and returns snapshots for the
effective unit list. A new query mode is added:

```text
GET /v1/units?all=1
```

When `all=1`, servicectl builds a normalized union of:

1. service unit files discovered in configured systemd unit paths;
2. enabled units;
3. runner units that represent runtime participation;
4. effective units, including units expanded from enabled groups.

Names are deduplicated after stripping one `.service` suffix and sorted before
snapshot collection. The response remains the existing structured
`visionapi.UnitsResponse`:

```json
{
  "generated_at": "2026-07-17T00:00:00Z",
  "units": [
    {
      "name": "example",
      "description": "Example service",
      "state": "STARTED",
      "phase": "ready"
    }
  ]
}
```

The backend adds `.service` when a returned name has no unit suffix. Internal
dinit, s6, logger, notify wrapper, and orchestrator names are not returned by
this endpoint unless they have their own application unit file.

## Backend ABI

The backend ABI gains a catalog item and two function pointers at the end of the
existing size-versioned structures:

```c
typedef struct Systemd1BrokerBackendUnit {
        size_t size;
        const char *id;
        const char *description;
        Systemd1BrokerBackendState state;
} Systemd1BrokerBackendUnit;

typedef struct Systemd1BrokerBackendOps {
        size_t size;
        void *userdata;
        int (*status)(...);
        int (*start)(...);
        int (*stop)(...);
        int (*list_units)(void *userdata,
                          Systemd1BrokerBackendUnit **ret_units,
                          size_t *ret_n_units);
        void (*free_units)(void *userdata,
                           Systemd1BrokerBackendUnit *units,
                           size_t n_units);
} Systemd1BrokerBackendOps;
```

`list_units` returns one complete snapshot. A successful empty result means the
backend currently knows no units. A negative errno-style result means no new
snapshot is available.

The backend owns the returned array and all strings reachable through it. The
broker treats the data as read-only and calls `free_units` exactly once after it
has copied the snapshot. This avoids allocator ownership assumptions across a
shared-library boundary.

Every item must have `size >= sizeof(Systemd1BrokerBackendUnit)`, a valid unit
id, and a valid backend state. The broker rejects the whole snapshot if any item
is malformed. Duplicate ids are also rejected rather than merged silently.

`list_units` and `free_units` become required operations for the current ABI.
The broker and its shipped backend library are deployed together; no compatibility
shim is added for older, unshipped backend modules.

## servicectl Backend

The standalone servicectl backend queries the servicectl control-plane Unix
socket using `GET /v1/units?all=1`. The socket path has a deployment default and
an environment override for tests, following the existing SysVision status
client pattern.

The backend:

1. applies the existing HTTP response size and I/O timeout limits;
2. rejects non-200 responses and malformed JSON;
3. validates every returned name;
4. maps each UnitSnapshot to the existing init-neutral backend state;
5. copies names and descriptions into one backend-owned result allocation;
6. releases the entire result through `free_units`.

Per-unit status continues to use SysVision. Catalog enumeration does not spawn
the `servicectl` CLI and does not link to libsystemd.

## Broker Synchronization

Each broker unit records a backend catalog generation. Generation zero means the
unit was added outside a successful backend catalog snapshot, for example by
`LoadUnit` or `StartUnit`.

On successful synchronization the broker:

1. validates the complete backend snapshot before changing manager state;
2. increments the catalog generation;
3. adds units that are not present;
4. updates descriptions and backend states for existing units;
5. marks every returned unit with the new generation;
6. removes units from older backend generations when they have no active job.

An on-demand unit that later appears in enumeration becomes catalog-managed. An
on-demand unit that has never appeared in enumeration remains available and is
not removed by catalog reconciliation.

If a disappeared catalog unit still has a job, removal is deferred. The next
successful synchronization removes it after the job is gone if it remains absent.

All reconciliation is prepared before mutation where allocation can fail. A
validation or allocation failure leaves the previous catalog intact.

## Synchronization Points

The broker performs an initial synchronization after loading the backend and
before exporting the Manager interface. Failure of this initial synchronization
is fatal because an empty initial catalog would falsely claim that no units are
known.

The broker synchronizes again immediately before serving:

1. `ListUnits()`;
2. `ListUnitsFiltered()`;
3. `ListUnitsByPatterns()`;
4. `ListUnitsByNames()`.

All four methods then filter the same manager snapshot. The broker does not call
the backend once per unit during a list operation.

After the first successful snapshot, a later backend error is logged and the
request is served from the last complete catalog. This gives callers a stale but
internally consistent view instead of an empty or partially updated list.

`GetUnit()` and `LoadUnit()` retain per-unit on-demand behavior. `StartUnit()` may
also create an on-demand unit before starting it, as required for D-Bus activation.

## systemctl Semantics

The broker exposes all synchronized units through the Manager list APIs.
Systemctl's normal state filters determine which units appear in the default
display. `systemctl list-units --all` includes inactive units because they remain
in the synchronized broker catalog with `inactive/dead` state.

Filtering by state, name, or pattern happens in the broker after synchronization.
The backend API has no systemd-specific filter strings.

## Failure Handling

1. Initial enumeration failure prevents broker startup.
2. Later enumeration failure preserves and serves the last successful catalog.
3. Malformed or duplicate backend entries reject the entire new snapshot.
4. A missing description becomes the unit id; it does not reject the snapshot.
5. A unit that disappears while a job is active remains until a later successful
   synchronization.
6. Backend result memory is released on every success path, including validation
   failure after `list_units` returns.

## Validation

Servicectl tests verify:

1. `/v1/units` retains effective-list behavior;
2. `/v1/units?all=1` returns the union of discovered, enabled, runner, and
   effective units;
3. duplicate suffix forms are normalized and output is sorted;
4. runtime-only and inactive discovered units both receive snapshots.

Backend tests verify:

1. the request uses `/v1/units?all=1`;
2. multiple snapshots decode into ABI catalog entries;
3. active, inactive, transitional, failed, and absent states map correctly;
4. malformed JSON, invalid names, duplicate names, oversized responses, and HTTP
   errors fail the whole enumeration;
5. the library still links only to libc and exports only its ABI entry point.

Broker tests verify:

1. initial synchronization adds all fake backend units;
2. a later snapshot adds, updates, and removes units atomically;
3. an active job defers removal;
4. a failed refresh retains the previous catalog;
5. on-demand units are not removed unless they became catalog-managed;
6. every `ListUnits*()` method refreshes before applying its filters;
7. the production executable no longer injects `alpha.service`.

Live validation compares normalized unit sets from:

```sh
servicectl list --json
systemctl list-units --all --no-legend --plain
```

It also confirms default `systemctl list-units` hides inactive units while
`--all` includes them.

## Implementation Order

1. Extend servicectl `/v1/units` tests and implement `all=1`.
2. Extend the backend ABI and fake backend tests.
3. Implement servicectl backend catalog decoding and ownership cleanup.
4. Implement atomic broker catalog synchronization.
5. Wire initial and pre-list synchronization.
6. Remove the static `alpha.service` production seed.
7. Run focused tests and live list comparison.
