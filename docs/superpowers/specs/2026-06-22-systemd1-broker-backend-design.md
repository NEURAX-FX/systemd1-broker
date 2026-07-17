# systemd1 Broker Backend Core Design

## Purpose

Define the backend boundary for `systemd1-broker` so the broker can expose a
systemd-compatible D-Bus surface while controlling non-systemd service managers
through a small, init-neutral contract.

The broker remains responsible for systemd compatibility, unit metadata,
dependency ordering, jobs, D-Bus signals, and systemd state strings. Backends are
responsible only for querying, starting, and stopping one already-resolved unit.

## Goals

1. Keep backend implementations small enough for `servicectl`, `s6`, `dinit`, and
   future managers to share the same contract.
2. Avoid leaking systemd-specific concepts such as `ActiveState`, `SubState`,
   `reload`, `restart`, transactions, or D-Bus object paths into backend code.
3. Let the broker parse unit metadata once and pass normalized execution data to
   the backend.
4. Let the broker compute dependency order consistently across all backends.
5. Make `systemctl start`, `stop`, `restart`, `reload`, `status`, `show`, and
   `list-units` work through the existing compatibility facade.

## Non-Goals

1. Recreating systemd's complete transaction engine.
2. Making the backend parse systemd unit files.
3. Adding backend-specific D-Bus APIs.
4. Supporting backend-managed dependency expansion in the first implementation.
5. Adding a backend `Reload` or `Restart` operation to the core contract.

## Decisions

1. The backend core exposes `ListUnits`, `Status`, `Start`, and `Stop`.
2. `ReloadUnit` is handled by the broker compatibility layer. It refreshes broker
   metadata and returns a successful broker job if the refresh succeeds.
3. `RestartUnit` is handled by the broker as an ordered `Stop` followed by
   `Start` for the target unit and any dependencies selected by broker policy.
4. Dependency ordering is computed by the broker. A backend receives one unit at a
   time and does not expand `Requires=`, `Wants=`, `Before=`, or `After=`.
5. Backend state is init-neutral. The D-Bus facade maps it to systemd-compatible
   `ActiveState` and `SubState` strings.

## Architecture

The backend path has these layers:

1. **D-Bus facade:** Accepts systemd-shaped methods and creates broker jobs.
2. **Broker scheduler:** Resolves the requested unit, computes dependency order,
   and decomposes high-level operations into backend `Start` and `Stop` calls.
3. **Metadata catalog:** Provides normalized unit metadata and execution details.
4. **Backend adapter:** Implements `ListUnits`, `Status`, `Start`, and `Stop` for
   a concrete service manager.

Only the D-Bus facade uses systemd method names and systemd state strings. Only
the backend adapter knows how to talk to `servicectl`, `s6`, `dinit`, or another
manager.

## Core Interface

The implementation may be in C function pointers, an out-of-process local
protocol, or a thin bridge to a Go service. The semantic contract is:

```c
typedef enum Systemd1BrokerBackendState {
        SYSTEMD1_BROKER_BACKEND_UNKNOWN,
        SYSTEMD1_BROKER_BACKEND_STARTING,
        SYSTEMD1_BROKER_BACKEND_RUNNING,
        SYSTEMD1_BROKER_BACKEND_STOPPING,
        SYSTEMD1_BROKER_BACKEND_STOPPED,
        SYSTEMD1_BROKER_BACKEND_FAILED,
} Systemd1BrokerBackendState;

typedef struct Systemd1BrokerBackend Systemd1BrokerBackend;
typedef struct Systemd1BrokerUnitExtra Systemd1BrokerUnitExtra;

int (*list_units)(Systemd1BrokerBackend *backend,
                  Systemd1BrokerBackendUnit **ret_units,
                  size_t *ret_n_units);

int (*status)(Systemd1BrokerBackend *backend,
              const char *unit_name,
              const Systemd1BrokerUnitExtra *extra,
              Systemd1BrokerBackendState *ret_state);

int (*start)(Systemd1BrokerBackend *backend,
             const char *unit_name,
             const Systemd1BrokerUnitExtra *extra);

int (*stop)(Systemd1BrokerBackend *backend,
            const char *unit_name,
            const Systemd1BrokerUnitExtra *extra);
```

The exact C names can change during implementation. The operation set and
ownership boundary should not change without updating this design. Catalog
ownership and reconciliation are specified in
`docs/superpowers/specs/2026-07-17-systemd1-broker-unit-catalog-sync-design.md`.

## Unit Extra

`Systemd1BrokerUnitExtra` is broker-owned normalized metadata for one unit. A
backend must treat it as read-only input.

Required fields:

1. `type`: normalized service type such as `simple`, `oneshot`, `forking`, or an
   init-neutral fallback.
2. `exec_start`: argv/env/cwd/user/group data needed to start the service when the
   backend executes commands directly.
3. `exec_stop`: optional stop command data when available.
4. `environment`: merged environment visible to the managed process.
5. `cgroup`: broker-selected cgroup or accounting scope when available.
6. `dependencies`: parsed dependency metadata for status display and diagnostics.
7. `target`: grouping or target information selected by the broker.
8. `backend_id`: optional backend-native service id if the catalog maps the unit
   to a non-unit-file service name.

The backend may ignore fields it cannot support, but it must not reinterpret
dependency metadata as permission to start or stop additional units.

## Operation Semantics

`Status(unit, extra)` returns the backend's current view of one unit. It must not
start, stop, reload, or repair the unit.

`ListUnits()` returns one complete init-neutral catalog snapshot. It must not
start, stop, reload, or repair any unit.

`Start(unit, extra)` requests that one unit become running. It may return after
the request is accepted or after completion, depending on backend capability. The
broker records the job and uses later `Status` calls or backend events to publish
state changes.

`Stop(unit, extra)` requests that one unit stop. It follows the same completion
model as `Start`.

Backends should return negative errno-style errors in in-process form. An
out-of-process adapter should preserve the same error categories.

## Reload And Restart

`ReloadUnit(name, mode)` is broker-local. It refreshes unit metadata, property
catalogs, and status snapshots for the requested unit. It does not call backend
`Start` or `Stop`, and it does not require backend reload support.

`Reload()` on the Manager performs the same kind of metadata refresh globally.

`RestartUnit(name, mode)` is broker-composed. The broker creates one restart job,
then executes the selected stop/start sequence through backend `Stop` and
`Start`. The public job remains `restart`; the backend only sees `Stop` and
`Start`.

`ReloadOrRestartUnit(name, mode)` uses broker-local reload only when the requested
compatibility behavior is metadata refresh. If service process re-execution is
needed, it follows the broker-composed restart path.

## Dependency Ordering

The broker computes dependency order before invoking the backend. The first
implementation uses a conservative subset:

1. For start, order hard requirements before the requested unit when metadata is
   available.
2. For stop, order dependent units before the requested unit when the broker can
   prove the relationship.
3. Ignore dependency types that are parsed but not implemented, with diagnostics
   rather than silent reinterpretation.
4. Reject cycles that affect the requested operation with a deterministic job
   failure.
5. Call backend `Start` or `Stop` once per selected unit in the computed order.

The backend must not independently traverse dependencies. Backend-native service
managers that already apply their own dependencies may still be supported, but
the adapter must document that behavior because it can duplicate broker work.

## State Mapping

The backend state maps to systemd-facing properties in the D-Bus adapter:

| Backend state | `ActiveState` | `SubState` |
|---------------|---------------|------------|
| `unknown` | `inactive` | `dead` |
| `starting` | `activating` | `start` |
| `running` | `active` | `running` |
| `stopping` | `deactivating` | `stop` |
| `stopped` | `inactive` | `dead` |
| `failed` | `failed` | `failed` |

The core state enum intentionally has no `reloading` value. A temporary
systemd-facing `reloading` state may be synthesized by the D-Bus facade only for
a broker-local reload job.

## Job Lifecycle

The broker owns all systemd-compatible jobs:

1. Validate the request and mode.
2. Resolve unit metadata and dependency order.
3. Create a broker job with the public type requested by the client.
4. Invoke backend `Start` or `Stop` for each selected unit.
5. Refresh status snapshots.
6. Emit Unit `PropertiesChanged` before `JobRemoved`.
7. Complete the public job with `done`, `failed`, `timeout`, `dependency`, or
   another supported systemd-compatible result.

Backend events can update units without a broker job. In that case the broker
emits Unit `PropertiesChanged` but does not synthesize `JobNew` or `JobRemoved`.

## Error Mapping

Backend errors are mapped by the broker:

| Backend condition | Broker result |
|-------------------|---------------|
| unit not known to backend | `org.freedesktop.systemd1.NoSuchUnit` or job `failed` |
| operation unsupported | `org.freedesktop.DBus.Error.NotSupported` |
| permission denied | `org.freedesktop.DBus.Error.AccessDenied` |
| operation timeout | job `timeout` |
| dependency ordering failed | job `dependency` |
| backend command failed | job `failed` |

The broker may include backend diagnostic text in status output, but clients must
not depend on exact wording.

## servicectl Adapter Shape

The first real adapter should target `servicectl` because it already has an
init-neutral control surface and SysVision/OrchestrD/PropertyD side channels.

Expected mapping:

1. `ListUnits` reads all known units from the structured servicectl API.
2. `Status` reads a unit's current phase from the servicectl status/property
   path, preferring existing SysVision state when available.
3. `Start` sends the existing servicectl start request for one resolved unit.
4. `Stop` sends the existing servicectl stop request for one resolved unit.
5. Broker dependency ordering remains outside servicectl for the broker path.
6. Extra metadata is passed to servicectl only when servicectl needs it to execute
   units not already known to its own catalog.

This keeps the systemd1 broker as a compatibility facade rather than a second
implementation of servicectl orchestration.

## Validation

Focused backend-interface tests should verify:

1. `StartUnit` calls backend `Start` for the requested unit after dependency
   ordering.
2. `StopUnit` calls backend `Stop` for the requested unit after dependency
   ordering.
3. `RestartUnit` creates one public restart job but calls backend `Stop` then
   `Start`.
4. `ReloadUnit` creates or completes a broker-local reload job without invoking
   backend operations.
5. Backend `running`, `stopped`, and `failed` states map to expected
   `ActiveState` and `SubState` values.
6. A backend event updates Unit properties without creating a broker job.
7. Dependency cycles fail before any backend operation is invoked.

Smoke validation should use `systemctl` against the broker socket:

```sh
systemctl status example.service
systemctl start example.service
systemctl stop example.service
systemctl restart example.service
systemctl reload example.service
systemctl show example.service -p ActiveState -p SubState -p Job
```

## Implementation Phases

1. Define the backend core structs, enum, and function-pointer table in the broker
   source tree.
2. Add a fake backend for focused tests that records calls and returns configured
   states.
3. Move current fake start/stop/restart behavior behind broker-composed job
   helpers.
4. Implement broker-local reload behavior without backend calls.
5. Add dependency ordering for the minimal supported metadata subset.
6. Add the first `servicectl` adapter once the interface is covered by tests.

## Open Items

1. Exact `Systemd1BrokerUnitExtra` field names and ownership rules should be
   finalized when the C structs are introduced.
2. The first dependency subset must be chosen from metadata already parsed by the
   broker to avoid designing ahead of implementation.
3. The `servicectl` adapter should decide whether it is in-process C, a local
   IPC bridge, or a separate broker-to-servicectl command path after measuring the
   existing servicectl APIs.
