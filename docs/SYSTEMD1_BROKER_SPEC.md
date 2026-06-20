---
title: systemd1 Broker Compatibility Layer
category: Documentation for Developers
layout: default
SPDX-License-Identifier: LGPL-2.1-or-later
---

# systemd1 Broker Compatibility Layer

This document specifies a small compatibility daemon that exposes enough of the
`org.freedesktop.systemd1` D-Bus API for `systemctl` and related tooling to
control a non-systemd service manager.

The broker is not a replacement for `systemd` as PID 1. It is a D-Bus and
private-socket compatibility layer with a narrow service-management model behind
it.

## Problem Statement

Many tools use `systemctl` as their control interface even when the desired
backend is not the full systemd service manager. Reimplementing all of systemd is
not a practical compatibility strategy: the hard part is not the transport, but
the precise Manager, Unit, Job, unit-file, and signal semantics expected by
clients.

The broker should make the common operational subset work:

1. `systemctl list-units`
2. `systemctl status UNIT`
3. `systemctl start UNIT`
4. `systemctl stop UNIT`
5. `systemctl restart UNIT`
6. `systemctl is-active UNIT`
7. `systemctl show UNIT`
8. `systemctl --wait start UNIT`

Compatibility with uncommon systemd features is intentionally staged rather than
attempted up front.

## Goals

1. Expose `org.freedesktop.systemd1` on the expected bus name and object paths.
2. Accept `systemctl` clients that use either the well-known bus name or the
   systemd private socket.
3. Maintain an in-memory Unit and Job model that can be translated to and from a
   backend service manager.
4. Emit the D-Bus signals required for `systemctl` state refresh and job waiting.
5. Keep the backend service manager behind a small adapter interface.
6. Support a metadata index for unit names, descriptions, dependencies, and
   install state without implementing all systemd execution semantics.

## Non-Goals

1. Running as PID 1.
2. Implementing cgroup ownership, resource control, device policy, namespaces,
   credentials, mount propagation, or socket activation in the broker itself.
3. Parsing and executing the complete systemd unit language.
4. Supporting every `systemctl` verb in the first implementation.
5. Reusing systemd's core `Manager` implementation as a library.
6. Providing bug-for-bug compatibility for unsupported properties and methods.

## Existing systemd Touchpoints

`systemctl` normally obtains its connection through `acquire_bus()`. For local
system mode it prefers the private manager socket before falling back to the
standard bus path. The compatibility daemon therefore needs to provide the same
entry points:

1. System private socket: `/run/systemd/private`
2. User private socket: `$XDG_RUNTIME_DIR/systemd/private`
3. Bus name: `org.freedesktop.systemd1`
4. Manager object: `/org/freedesktop/systemd1`
5. Unit objects: `/org/freedesktop/systemd1/unit/<escaped-unit-name>`
6. Job objects: `/org/freedesktop/systemd1/job/<id>`

The real systemd daemon registers the same D-Bus object model for both normal
bus connections and private socket clients. The broker should follow that model
rather than adding a separate protocol.

## Architecture

The broker has four layers:

1. **Transport:** Accepts sd-bus clients on the private socket and optionally
   owns `org.freedesktop.systemd1` on the system or user bus.
2. **D-Bus facade:** Implements Manager, Unit, Job, and Properties interfaces
   with systemd-compatible signatures.
3. **State model:** Stores normalized units, jobs, dependencies, and install
   metadata independent of any backend.
4. **Backend adapter:** Starts, stops, restarts, reloads, and queries services
   through the selected service manager.

The D-Bus facade must not leak backend-specific state. Backend-specific states
are mapped into systemd-compatible `LoadState`, `ActiveState`, `SubState`,
`UnitFileState`, and Job result strings.

## Prototype Status

The current prototype implements the narrow in-memory and D-Bus slice needed to
exercise the object model without a real service-manager backend:

1. Unit and Job tuple encoding for `ListUnits()` and `ListJobs()` using the exact
   `a(ssssssouso)` and `a(usssoo)` shapes.
2. An in-memory Manager/Unit/Job model with escaped Unit and Job object paths,
   basic backend-state mapping, read-only listing helpers, and fake start/stop/
   restart job completion semantics.
3. A minimal sd-bus facade for Manager `GetUnit()`, `LoadUnit()`, `ListUnits()`,
   `ListUnitsFiltered()`, `ListUnitsByNames()`, `ListUnitsByPatterns()`,
   `ListJobs()`, `StartUnit()`, `StopUnit()`, `ReloadUnit()`, `RestartUnit()`,
   `TryRestartUnit()`, `Subscribe()`, and `Unsubscribe()`.
4. Minimal Manager, Unit, Service, and Job object vtables, including `GetAll()`
   coverage for the supported neutral property values.
5. `JobNew` signal emission when D-Bus operations create jobs, plus `JobRemoved`
   and Unit `PropertiesChanged` signal emission when a controlled test operation
   completes a fake broker job.
6. A `systemd1-broker --socket=PATH` test executable that seeds `alpha.service`
   and serves direct sd-bus clients over a Unix stream socket.

The prototype intentionally does not yet implement bus-name ownership, system or
user private socket path management, reload-capable backend operations,
unit-file indexing, install-state metadata, authorization policy, automatic
backend-driven job completion and state-change signal emission, or a real backend
adapter.

## Transport Requirements

The broker listens on a Unix stream socket compatible with systemd's private bus
transport. Clients connecting to the private socket speak D-Bus directly over the
accepted connection.

For system mode, the socket path is `/run/systemd/private`. For user mode, the
socket path is `$XDG_RUNTIME_DIR/systemd/private`. The broker must create the
parent directory if it owns the runtime directory for this compatibility mode,
and it must fail clearly if another manager already owns the socket.

The broker may also connect to the system or user bus and request
`org.freedesktop.systemd1`. This is required for clients that do not use the
private socket path.

## D-Bus Surface: Manager

The first implementation supports this subset of
`org.freedesktop.systemd1.Manager`:

| Member | Type | Required behavior |
|--------|------|-------------------|
| `ListUnits()` | method | Return all loaded units using the systemd tuple shape. |
| `ListUnitsFiltered(as states)` | method | Return loaded units matching active/load states. |
| `GetUnit(s name)` | method | Return the unit object path if known, otherwise an error. |
| `LoadUnit(s name)` | method | Load metadata if available and return the unit object path. |
| `StartUnit(s name, s mode)` | method | Create a start job and return its object path. |
| `StopUnit(s name, s mode)` | method | Create a stop job and return its object path. |
| `RestartUnit(s name, s mode)` | method | Create a restart job and return its object path. |
| `ReloadUnit(s name, s mode)` | method | Create a reload job when the backend supports reload. |
| `TryRestartUnit(s name, s mode)` | method | Restart only active units. |
| `ReloadOrRestartUnit(s name, s mode)` | method | Prefer reload if available, otherwise restart. |
| `ListJobs()` | method | Return active broker jobs using the systemd tuple shape. |
| `Subscribe()` | method | Enable compatibility signal delivery for the connection. |
| `Unsubscribe()` | method | Disable compatibility signal delivery for the connection. |
| `Reload()` | method | Reload broker metadata and backend state. |

The broker must advertise and implement the exact method signatures used by
systemd for this subset:

| Member | Input signature | Output signature |
|--------|-----------------|------------------|
| `GetUnit` | `s` | `o` |
| `LoadUnit` | `s` | `o` |
| `StartUnit` | `ss` | `o` |
| `StopUnit` | `ss` | `o` |
| `ReloadUnit` | `ss` | `o` |
| `RestartUnit` | `ss` | `o` |
| `TryRestartUnit` | `ss` | `o` |
| `ReloadOrRestartUnit` | `ss` | `o` |
| `ListUnits` | none | `a(ssssssouso)` |
| `ListUnitsFiltered` | `as` | `a(ssssssouso)` |
| `ListUnitsByNames` | `as` | `a(ssssssouso)` |
| `ListUnitsByPatterns` | `asas` | `a(ssssssouso)` |
| `ListJobs` | none | `a(usssoo)` |
| `Subscribe` | none | none |
| `Unsubscribe` | none | none |
| `Reload` | none | none |

The `ListUnits` tuple fields are, in order: unit id, description, load state,
active state, sub state, following unit id, unit object path, job id, job type,
and job object path. When no job is attached, the job id is zero, the job type is
the empty string, and the job object path is `/`.

The `ListJobs` tuple fields are, in order: job id, unit id, job type, job state,
job object path, and unit object path.

Unsupported Manager methods return a D-Bus error. Prefer
`org.freedesktop.DBus.Error.NotSupported` when the method exists but is outside
the broker's scope, and `org.freedesktop.DBus.Error.UnknownMethod` only for
members the broker does not advertise.

The first implementation supports these Manager properties:

| Property | Type | Required behavior |
|----------|------|-------------------|
| `Version` | `s` | Broker version string. |
| `Features` | `s` | Space-separated feature markers; may be empty. |
| `Virtualization` | `s` | Empty unless known. |
| `Architecture` | `s` | Host architecture if available. |
| `NNames` | `u` | Number of known units. |
| `NJobs` | `u` | Number of active jobs. |
| `Environment` | `as` | Broker environment visible to managed services if meaningful; otherwise empty. |
| `ControlGroup` | `s` | Empty unless the backend exposes a manager cgroup. |
| `SystemState` | `s` | `running`, `degraded`, `starting`, or `offline` based on backend health. |

## D-Bus Surface: Unit

Every known unit is exported at a stable escaped object path. The broker supports
`org.freedesktop.systemd1.Unit` and `org.freedesktop.DBus.Properties`.

The first implementation supports these Unit properties:

| Property | Type | Required behavior |
|----------|------|-------------------|
| `Id` | `s` | Canonical unit name. |
| `Names` | `as` | Primary name plus aliases. |
| `Following` | `s` | Empty unless the unit aliases another active unit. |
| `Requires` | `as` | Parsed or backend-provided hard dependencies. |
| `Wants` | `as` | Parsed or backend-provided soft dependencies. |
| `Conflicts` | `as` | Parsed or backend-provided conflicts. |
| `Before` | `as` | Ordering dependencies known to the broker. |
| `After` | `as` | Ordering dependencies known to the broker. |
| `Description` | `s` | Human-readable description. |
| `LoadState` | `s` | `loaded`, `not-found`, `error`, or `masked`. |
| `ActiveState` | `s` | `active`, `inactive`, `activating`, `deactivating`, `failed`, or `reloading`. |
| `SubState` | `s` | Type-specific state mapped from the backend. |
| `FragmentPath` | `s` | Unit file path if known. |
| `SourcePath` | `s` | Source metadata path if distinct from `FragmentPath`. |
| `DropInPaths` | `as` | Drop-in paths included in metadata, or empty. |
| `UnitFileState` | `s` | `enabled`, `disabled`, `static`, `masked`, or empty. |
| `NeedDaemonReload` | `b` | True when indexed metadata is stale. |
| `Job` | `(uo)` | Active job id and path, or zero and `/`. |
| `InvocationID` | `ay` | Empty unless the backend exposes an invocation id. |

For `systemctl status UNIT` to be useful for service units, the broker also
exports a minimal `org.freedesktop.systemd1.Service` interface for `.service`
units. These properties must be present with the correct type, even when the
backend can only return neutral values:

| Property | Type | Neutral value |
|----------|------|---------------|
| `MainPID` | `u` | `0` |
| `ExecMainPID` | `u` | `0` |
| `ControlPID` | `u` | `0` |
| `Result` | `s` | `success` or empty when unknown |
| `StatusText` | `s` | empty string |
| `StatusErrno` | `i` | `0` |
| `StatusBusError` | `s` | empty string |
| `StatusVarlinkError` | `s` | empty string |
| `ExecMainStartTimestamp` | `t` | `0` |
| `ExecMainExitTimestamp` | `t` | `0` |
| `ExecMainCode` | `i` | `0` |
| `ExecMainStatus` | `i` | `0` |
| `PIDFile` | `s` | empty string |
| `LogNamespace` | `s` | empty string |

The first implementation supports these Unit methods:

| Member | Required behavior |
|--------|-------------------|
| `Start(s mode)` | Equivalent to Manager `StartUnit()`. |
| `Stop(s mode)` | Equivalent to Manager `StopUnit()`. |
| `Restart(s mode)` | Equivalent to Manager `RestartUnit()`. |
| `Reload(s mode)` | Equivalent to Manager `ReloadUnit()`. |
| `ResetFailed()` | Clear failed state if the backend supports it; otherwise clear broker-local failure. |

Other unit-type-specific interfaces such as `Socket`, `Timer`, `Mount`, and
`Target` are optional in the first version. If exported, they must only expose
properties the broker can keep accurate. It is better to omit a type-specific
interface than to return fabricated values.

## D-Bus Surface: Job

Every asynchronous operation returns a Job object path. This is required because
`systemctl --wait` waits for job completion signals rather than treating method
return as operation completion.

The first implementation supports `org.freedesktop.systemd1.Job` with these
properties:

| Property | Type | Required behavior |
|----------|------|-------------------|
| `Id` | `u` | Broker-local monotonically increasing job id. |
| `Unit` | `(so)` | Unit name and object path. |
| `JobType` | `s` | `start`, `stop`, `restart`, `reload`, or `verify-active`. |
| `State` | `s` | `waiting` or `running` while the job exists. |

The `Cancel()` method cancels a pending or running backend operation when the
adapter supports cancellation. If cancellation is not supported, it returns
`org.freedesktop.DBus.Error.NotSupported`.

Completed jobs are not reported as `State=done`. Completion is represented by a
single Manager `JobRemoved` signal with a result string. The broker may keep a
completed job object alive briefly only to flush pending D-Bus signals; it must
not appear in `ListJobs()` after completion.

The first implementation accepts `replace` and `fail` job modes. Unsupported job
modes, including isolation and dependency-ignoring modes, return
`org.freedesktop.DBus.Error.NotSupported` unless the backend can implement their
systemd semantics exactly. Starting a new job for a unit with an incompatible
running job returns `org.freedesktop.systemd1.TransactionIsDestructive` unless
the requested mode allows replacement.

## Signals

The broker emits these Manager signals:

| Signal | Required behavior |
|--------|-------------------|
| `UnitNew(s id, o path)` | Emitted when a unit first becomes known. |
| `UnitRemoved(s id, o path)` | Emitted when a unit disappears from the broker index. |
| `JobNew(u id, o path, s unit)` | Emitted after a job is created. |
| `JobRemoved(u id, o path, s unit, s result)` | Emitted exactly once for every job. |

The broker emits `org.freedesktop.DBus.Properties.PropertiesChanged` for Unit
state changes. At minimum, changes to `LoadState`, `ActiveState`, `SubState`,
`UnitFileState`, `NeedDaemonReload`, and `Job` are signalled.

Job results use systemd-compatible strings where possible: `done`, `canceled`,
`timeout`, `failed`, `dependency`, `skipped`, and `invalid`.

For a broker-initiated operation, the signal order is fixed:

1. `JobNew` after the job is visible through `ListJobs()` and the Unit `Job`
   property.
2. Unit `PropertiesChanged` for state changes caused by backend progress.
3. Job `PropertiesChanged` when the job moves from `waiting` to `running`.
4. Final Unit `PropertiesChanged` for the stable state.
5. `JobRemoved` exactly once, after the final Unit state is observable.

## Unit Metadata Index

The metadata index is intentionally smaller than systemd's full unit loader. It
provides enough information for listing, status display, basic dependency
inspection, and install-state reporting.

The indexer reads unit files and drop-ins from a configured search path. The
default search path follows systemd conventions for the selected scope:

1. System persistent configuration, for example `/etc/systemd/system`.
2. System runtime configuration, for example `/run/systemd/system`.
3. Vendor unit directories, for example `/usr/lib/systemd/system`.
4. User configuration and runtime directories for user mode.

The first implementation parses this minimal key set:

| Section | Keys |
|---------|------|
| `[Unit]` | `Description=`, `Requires=`, `Wants=`, `Conflicts=`, `Before=`, `After=`, `Documentation=` |
| `[Service]` | `Type=`, `ExecStart=`, `ExecReload=`, `Restart=` |
| `[Install]` | `WantedBy=`, `RequiredBy=`, `Alias=` |

The dependency parser should treat these keys as first-class metadata when they
appear, even if the backend does not act on them: `Requisite=`, `BindsTo=`,
`PartOf=`, `Upholds=`, `OnFailure=`, `OnSuccess=`, `PropagatesReloadTo=`,
`ReloadPropagatedFrom=`, `PropagatesStopTo=`, `StopPropagatedFrom=`,
`JoinsNamespaceOf=`, `RequiresMountsFor=`, and `WantsMountsFor=`. Unknown
dependency keys are ignored with diagnostics rather than silently folded into a
different dependency type.

Drop-ins are applied in systemd search-path order and lexical file order within
each drop-in directory. The index stores both the final merged metadata and the
contributing `DropInPaths`. A symlink to `/dev/null` marks the unit as masked and
sets `LoadState=masked` and `UnitFileState=masked`.

Aliases are resolved to one canonical Unit object. `Names` includes the primary
name and aliases, `GetUnit()` accepts any alias, and `ListUnits()` emits only the
canonical loaded unit entry.

The broker does not execute `ExecStart=` itself unless the selected backend
adapter explicitly uses unit files as its service definition. For most backends,
unit files are metadata only.

Unsupported keys are ignored with diagnostics. A unit with unsupported keys may
still be `loaded` if the backend can manage it. A parse error in a supported key
sets `LoadState=error` and records the diagnostic for status output.

The index tracks a generation number derived from source paths and mtimes. A
change to any known unit file or drop-in sets `NeedDaemonReload=true` until the
broker reloads metadata through Manager `Reload()` or a configured automatic
refresh path.

## Backend Adapter

The backend adapter is the only layer that knows how to control the real service
manager. Its contract is:

1. Enumerate known services and their current state.
2. Resolve a unit name to a backend service id.
3. Start, stop, restart, and optionally reload a service.
4. Report operation completion asynchronously.
5. Report service state changes independently of broker-initiated jobs.
6. Provide optional metadata such as description, dependencies, enabled state,
   process id, invocation id, and failure reason.

Each backend adapter advertises capabilities before it is used. The minimum
capability set is `enumerate`, `query-state`, `start`, `stop`, and `restart`.
Optional capabilities include `reload`, `cancel`, `reset-failed`, `main-pid`,
`invocation-id`, `enabled-state`, `dependencies`, and `journal-cursor`.

Backend operations are asynchronous from the facade's point of view. An adapter
operation returns either an immediate error before a Job is exposed, or an
operation handle that will later complete with one of the broker Job result
strings. The adapter must also provide a state snapshot after completion so the
broker can publish final Unit properties before emitting `JobRemoved`.

Independent backend state changes are treated as Unit state updates without a
broker Job. The broker emits Unit `PropertiesChanged` but does not synthesize
`JobNew` or `JobRemoved` for changes it did not initiate.

The D-Bus facade creates a Job before invoking the backend. The backend completes
the Job through a callback or event. The broker then updates Unit state and emits
`JobRemoved` and `PropertiesChanged` in a deterministic order:

1. Update internal Unit state.
2. Emit Unit `PropertiesChanged`.
3. Mark Job complete.
4. Emit Manager `JobRemoved`.
5. Remove or retain the completed Job according to the configured retention
   policy.

The facade never calls backend operations while holding D-Bus connection or state
model locks that a completion callback needs to re-enter. Adapter callbacks are
serialized onto the broker event loop before they mutate Unit or Job state.

## State Mapping

Backend state is mapped to systemd-compatible Unit state strings:

| Backend state | `ActiveState` | `SubState` |
|---------------|---------------|------------|
| Unknown or absent | `inactive` | `dead` |
| Starting | `activating` | `start` |
| Running | `active` | `running` |
| Reloading | `reloading` | `reload` |
| Stopping | `deactivating` | `stop` |
| Stopped cleanly | `inactive` | `dead` |
| Failed | `failed` | `failed` |

If a backend has richer state than this table, the adapter may provide a more
specific `SubState` as long as `ActiveState` remains one of the common systemd
states.

## Error Mapping

The broker maps backend errors to stable D-Bus errors:

| Condition | D-Bus error |
|-----------|-------------|
| Unit name is syntactically invalid | `org.freedesktop.systemd1.InvalidName` |
| Unit is unknown | `org.freedesktop.systemd1.NoSuchUnit` |
| Unit exists but backend cannot load it | `org.freedesktop.systemd1.LoadFailed` |
| Operation is not supported by adapter | `org.freedesktop.DBus.Error.NotSupported` |
| Operation conflicts with another running job | `org.freedesktop.systemd1.TransactionIsDestructive` |
| Caller lacks permission | `org.freedesktop.DBus.Error.AccessDenied` |
| Backend operation times out | `org.freedesktop.DBus.Error.Timeout` |
| Backend operation fails | `org.freedesktop.systemd1.JobFailed` |

The broker should include human-readable error messages, but clients must not
depend on exact message text.

## Security Model

Private-socket clients are local Unix peers. The broker records peer credentials
and applies policy before mutating backend state.

The minimal policy is:

1. Read-only methods are allowed for all local clients unless configured
   otherwise.
2. Mutating methods require root, the owning user in user mode, or an explicit
   broker policy grant.
3. Backend adapters may impose stricter policy.
4. The broker must not treat private-socket access alone as authorization for
   privileged operations.

Polkit integration is optional and can be added after the D-Bus surface is
stable.

## Implementation Language Boundary

The compatibility facade should be implemented in C with `sd-bus`. This keeps
the transport, object vtables, signatures, private socket behavior, and peer
credential handling close to the existing systemd client expectations.

Backends may be implemented in C or out of process. If the backend service
manager is written in Go, keep Go behind an adapter boundary rather than making
Go responsible for the systemd1 D-Bus compatibility surface.

This split avoids reimplementing low-level D-Bus details while still allowing the
service-management logic to live outside systemd.

## Phasing

### Phase 1: Read-Only Compatibility

1. Start broker in system or user mode.
2. Listen on the private socket.
3. Export Manager and Unit objects.
4. Implement `ListUnits()`, `ListUnitsFiltered()`, `ListUnitsByNames()`,
   `ListUnitsByPatterns()`, `GetUnit()`, `LoadUnit()`, and `GetAll()` for Manager
   and Unit properties.
5. Build a metadata index from backend enumeration and minimal unit-file parsing.
6. Verify `systemctl list-units`, `systemctl status UNIT`, `systemctl show UNIT`,
   and `systemctl is-active UNIT`.

### Phase 2: Mutating Jobs

1. Implement `StartUnit()`, `StopUnit()`, `RestartUnit()`, `ReloadUnit()`, and the
   matching Unit methods.
2. Add Job objects and monotonically increasing job ids.
3. Emit `JobNew`, `JobRemoved`, and Unit `PropertiesChanged`.
4. Verify `systemctl start UNIT`, `systemctl stop UNIT`, `systemctl restart UNIT`,
   and `systemctl --wait start UNIT`.

### Phase 3: Unit-File and Install Metadata

1. Implement install-state indexing for `enabled`, `disabled`, `static`, and
   `masked`.
2. Add `ListUnitFiles()` if client-side `systemctl list-unit-files` is not used
   for the deployment mode.
3. Add masked-unit handling.
4. Add stale metadata detection and `NeedDaemonReload`.

### Phase 4: Wider Compatibility

1. Add optional type-specific interfaces for `Service`, `Target`, `Socket`, and
   `Timer` where the backend can provide accurate state.
2. Add `ResetFailed()`, `KillUnit()`, and `SetUnitProperties()` only if they map
   cleanly to the backend.
3. Add bus-name ownership for clients that bypass the private socket.
4. Add policy integration and audit logging.

## Validation Plan

Use `systemctl` itself as the compatibility test client. Each test should run
against the broker socket and assert both command exit status and D-Bus signal
behavior.

Minimum command matrix:

```sh
systemctl list-units
systemctl status example.service
systemctl show example.service
systemctl is-active example.service
systemctl start example.service
systemctl --wait start example.service
systemctl stop example.service
systemctl restart example.service
```

Additional D-Bus-level tests should verify:

1. Object paths are stable and correctly escaped.
2. Manager methods use the exact expected signatures, especially
   `ListUnits()` returning `a(ssssssouso)` and `ListJobs()` returning
   `a(usssoo)`.
3. Unit `GetAll()` returns required properties with correct types.
4. Service `GetAll()` returns the minimal status properties with correct types
   for `.service` units.
5. Every mutating method that returns a Job emits exactly one `JobRemoved`.
6. Unit state changes emit `PropertiesChanged` before the corresponding
   `JobRemoved` signal.
7. Completed jobs disappear from `ListJobs()` after `JobRemoved`.
8. Unsupported methods return predictable D-Bus errors.

The focused command matrix should also include property-level checks:

```sh
systemctl show example.service -p Id -p LoadState -p ActiveState -p SubState -p Job
systemctl show example.service -p MainPID -p ExecMainPID -p Result -p StatusText
systemctl list-jobs
```

## Open Questions

1. Should the first target be system mode, user mode, or both?
2. Which backend service manager should define the first adapter contract?
3. Should unit-file parsing be metadata-only forever, or should some adapters use
   unit files as executable service definitions?
4. Should the broker own `org.freedesktop.systemd1` on the bus in phase 1, or
   should private-socket compatibility be proven first?
5. Which unsupported `systemctl` verbs should fail fast versus receive shim
   implementations?
