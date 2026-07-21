# systemd1 Broker Dynamic Metadata Implementation Plan

> **For OpenCode:** Implement this plan directly in the current session. Do not dispatch subagents. Follow the red-green-refactor sequence and stop on unexpected failures rather than weakening assertions.

**Goal:** Expose real backend-provided systemd Unit and Service metadata through `systemd1-broker`, so standard `systemctl show`, `systemctl status`, and `systemctl cat` display available values and omit unsupported optional properties.

**Architecture:** Servicectl adds one per-unit HTTP endpoint that returns a runtime snapshot and typed standard systemd properties. The libc-only servicectl backend decodes that response into an extended plugin ABI. The broker validates and atomically caches each snapshot, then a unit-path D-Bus callback handles dynamic Properties and introspection while existing vtables retain methods and broker-owned core properties.

**Tech Stack:** Go 1.x, net/http, JSON; GNU C17; systemd sd-json and sd-bus; Meson; GitHub Actions/mkosi.

**Design:** `docs/superpowers/specs/2026-07-21-systemd1-broker-dynamic-metadata-design.md`

## Repository Boundaries

This plan spans two existing worktrees:

1. Servicectl: `/root/servicectl`
2. Broker/systemd tree: `/root/systemd-src/worktrees/systemd1-broker`

Run commands with the matching working directory. Commit the Go endpoint in the servicectl repository before changing the broker ABI. Commit broker changes in small buildable stages. Never stage unrelated dirty-worktree changes.

## Task 1: Define The Servicectl Typed Metadata Response

**Files:**

- Modify: `/root/servicectl/internal/visionapi/types.go`
- Modify: `/root/servicectl/servicectl_api_test.go`

### Step 1: Write the failing response-shape test

Add a JSON round-trip test for these public API types:

```go
type SystemdProperty struct {
	Interface string          `json:"interface"`
	Name      string          `json:"name"`
	Signature string          `json:"signature"`
	Value     json.RawMessage `json:"value"`
}

type UnitDetailResponse struct {
	Unit              UnitSnapshot      `json:"unit"`
	SystemdProperties []SystemdProperty `json:"systemd_properties"`
}
```

The test must prove that a numeric `value` stays numeric and an array value stays an array after marshaling, rather than becoming a quoted JSON string.

### Step 2: Run the red test

Run:

```sh
go test . -run TestUnitDetailResponsePreservesTypedPropertyValues -count=1
```

Expected: compile failure because the new vision API types do not exist.

### Step 3: Add the API types

Import `encoding/json` in `internal/visionapi/types.go`, add the two structures adjacent to `UnitSnapshot`/`UnitsResponse`, and use `json.RawMessage` for `Value`. Do not add an untyped `any` field.

### Step 4: Run the green test

Run:

```sh
go test . -run TestUnitDetailResponsePreservesTypedPropertyValues -count=1
```

Expected: PASS.

## Task 2: Build One Rich Servicectl Unit Snapshot

**Files:**

- Modify: `/root/servicectl/servicectl_api.go`
- Modify: `/root/servicectl/servicectl_api_test.go`
- Read only unless a parser defect is found: `/root/servicectl/unit_parse.go`

### Step 1: Add test seams and failing property-builder tests

Add one server seam:

```go
buildUnitDetail func(Config, string, visionapi.UnitListsResponse) (visionapi.UnitDetailResponse, error)
```

Keep production work in a concrete `buildUnitDetailResponse` function. Add focused tests using a temporary unit directory and a controlled config. The fixture should contain:

```ini
[Unit]
Description=Demo worker
Requires=network.target
Wants=cache.service
After=network.target cache.service
Before=consumer.service

[Service]
Type=simple
ExecStart=/usr/bin/demo --serve
WorkingDirectory=/srv/demo
User=demo
Group=demo
```

Assert that the response includes exact typed values:

```text
Unit.FragmentPath      s                         "/tmp/.../demo.service"
Unit.UnitFileState     s                         "enabled" or "disabled"
Unit.Requires          as                        ["network.target"]
Unit.Wants             as                        ["cache.service"]
Unit.After             as                        sorted unit names
Unit.Before            as                        ["consumer.service"]
Service.Type           s                         "simple"
Service.ExecStart      a(sasbttttuii)            one compatible command record
Service.WorkingDirectory s                       "/srv/demo"
Service.User           s                         "demo"
Service.Group          s                         "demo"
```

Add a separate test where `MainPID`, status text, working directory, user, group, and optional dependencies are unavailable. Assert those keys are absent, not present with zero or empty values.

For `ExecStart`, encode the standard systemd signature exactly. Split the configured command into executable and argv using the same tokenization semantics already accepted by servicectl. Populate unavailable runtime fields in the tuple with their standard neutral values because they are fields inside a present structured property, not absent top-level properties.

### Step 2: Run the red tests

Run:

```sh
go test . -run 'TestBuildUnitDetail(ResponseIncludesStandardMetadata|OmitsUnavailableMetadata)' -count=1
```

Expected: compile or assertion failure because the builder does not exist.

### Step 3: Refactor snapshot collection to avoid duplicate parsing

Split the current `buildUnitSnapshot` implementation into:

```go
func buildUnitSnapshotFromParsed(cfg Config, unitName string, unit *Unit, socketUnit *SocketUnit) visionapi.UnitSnapshot
func buildUnitSnapshot(cfg Config, unitName string) (visionapi.UnitSnapshot, error)
```

The first helper must receive already parsed unit data and collect runtime state once. `buildUnitDetailResponse` must parse the unit once, parse the optional socket once, build the runtime snapshot once, and then derive metadata from those same objects.

Do not duplicate the global `config` swap. Keep it under `unitSnapshotConfigMu` for the whole parse-and-build operation.

### Step 4: Implement typed property construction

Add small constructors that marshal a concrete Go value into `json.RawMessage`:

```go
func systemdProperty(interfaceName, name, signature string, value any) (visionapi.SystemdProperty, error)
func appendSystemdProperty(properties []visionapi.SystemdProperty, interfaceName, name, signature string, value any) ([]visionapi.SystemdProperty, error)
```

Keep all property selection in `buildSystemdProperties(unit, snapshot, lists)`. Normalize dependency unit names to their original unit types; do not append `.service` to `.target`, `.socket`, or other named dependencies. Sort and deduplicate arrays for deterministic responses.

Map `UnitFileState` to `enabled` only when the canonical `.service` name occurs in `EnabledUnits`; otherwise publish `disabled` when the unit file was parsed successfully. Do not infer enablement from running state.

Parse `MainPID` only when it is a positive value within `uint32`; then publish both `MainPID` and `ExecMainPID`. Publish `StatusText` only when `snapshot.Status` is non-empty. Publish `Result="success"` for a known non-failed runtime result and `Result="exit-code"` for an observed failure; omit it when runtime outcome is unknown.

### Step 5: Run focused and regression tests

Run:

```sh
go test . -run 'TestBuildUnitDetail(ResponseIncludesStandardMetadata|OmitsUnavailableMetadata)|TestSnapshotIsRunning' -count=1
```

Expected: PASS.

## Task 3: Add The Per-Unit Servicectl HTTP Endpoint

**Files:**

- Modify: `/root/servicectl/servicectl_api.go`
- Modify: `/root/servicectl/servicectl_api_test.go`

### Step 1: Write failing route tests

Add table-driven tests for:

1. `GET /v1/units/demo` calls `buildUnitDetail` with `demo.service` and returns its response.
2. `GET /v1/units/demo.service` normalizes to one suffix.
3. An escaped valid name is decoded exactly once.
4. Missing names, nested paths, non-service suffixes, and names containing `/`, `\\`, whitespace, or NUL are HTTP 400.
5. A missing parsed unit is HTTP 404.
6. Internal build failures are HTTP 500.
7. Non-GET requests are HTTP 405.
8. Existing `GET /v1/units` catalog behavior remains unchanged.

The handler must query `UnitListsResponse` exactly once and pass it to the builder so `UnitFileState` uses the same request generation.

### Step 2: Run the red route tests

Run:

```sh
go test . -run 'TestServicectlAPIUnitDetail' -count=1
```

Expected: 404 from the current mux because no detail route exists.

### Step 3: Implement the route

Register `/v1/units/` after `/v1/units`. Use `r.PathValue` only if the repository's supported Go version already permits it; otherwise trim the exact prefix and use `url.PathUnescape`. Reject any decoded slash instead of treating it as another path segment.

Add a sentinel missing-unit error so HTTP status selection does not depend on matching error text. The canonical backend unit name in the response remains the normalized service name.

### Step 4: Verify the Go side

Run:

```sh
gofmt -w servicectl_api.go servicectl_api_test.go internal/visionapi/types.go
git diff --check
```

Expected: PASS and no formatting errors.

### Step 5: Commit the servicectl endpoint

Inspect `git status`, `git diff`, and recent log first. Stage only the three intended files and commit with the repository's normal style plus the required AI disclosure.

## Task 4: Extend The Backend ABI With Typed Snapshots

**Files:**

- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker-backend-api.h`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/test/test-systemd1-broker.c`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/test/test-systemd1-broker-backend-servicectl.c`

### Step 1: Add failing ABI presence tests

Extend `load_backend()` assertions and the fake backend context to require:

```c
Systemd1BrokerBackendProperty
Systemd1BrokerBackendUnitSnapshot
ops->get_unit_snapshot
ops->free_unit_snapshot
```

Add fake-backend ownership helpers that deep-copy every property string. Track `snapshot_calls`, `snapshot_error`, and `last_unit_name` separately from status calls.

### Step 2: Run the red build

Run:

```sh
meson test -C build -v test-systemd1-broker-backend-servicectl
```

Expected: compilation failure because the ABI members do not exist.

### Step 3: Add the ABI structs and operations

Append the design-approved structures and callbacks to `systemd1-broker-backend-api.h`. Do not insert fields into existing structures. `size` is the first field of every new ABI structure.

Update broker backend verification to require both new callbacks. Since this backend ABI has not shipped independently and the module is deployed with the broker, do not add old-size compatibility branches.

### Step 4: Rebuild to expose implementation failures

Run:

```sh
meson test -C build -v test-systemd1-broker-backend-servicectl
```

Expected: build succeeds far enough to fail because the servicectl backend does not populate the new operations.

## Task 5: Decode Per-Unit Metadata In The libc-Only Backend

**Files:**

- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker-backend-servicectl.c`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/test/test-systemd1-broker-backend-servicectl.c`

### Step 1: Write failing HTTP and ownership tests

Add tests that serve a response containing:

```json
{
  "unit": {
    "name": "demo",
    "description": "Demo",
    "state": "STARTED",
    "lifecycle": "ready"
  },
  "systemd_properties": [
    {"interface":"org.freedesktop.systemd1.Unit","name":"FragmentPath","signature":"s","value":"/etc/systemd/system/demo.service"},
    {"interface":"org.freedesktop.systemd1.Service","name":"MainPID","signature":"u","value":4242},
    {"interface":"org.freedesktop.systemd1.Unit","name":"After","signature":"as","value":["network.target"]},
    {"interface":"org.freedesktop.systemd1.Service","name":"ExecStart","signature":"a(sasbttttuii)","value":[["/usr/bin/demo",["/usr/bin/demo","--serve"],false,0,0,0,0,0,0,0]]}
  ]
}
```

Assert:

1. one request is `GET /v1/units/demo.service` with path escaping;
2. state and description come from the same response;
3. `value_json` preserves exact JSON types and nested structure;
4. every property has `size == sizeof(Systemd1BrokerBackendProperty)`;
5. `free_unit_snapshot` frees all nested allocations and accepts NULL;
6. HTTP 404 returns a successful ABSENT snapshot with no properties;
7. malformed/missing unit state, malformed property objects, bad top-level arrays, oversized responses, and non-2xx responses return errno-style errors without modifying output pointers.

### Step 2: Run the red tests

Run:

```sh
meson test -C build -v test-systemd1-broker-backend-servicectl
```

Expected: new snapshot tests fail.

### Step 3: Consolidate the HTTP client

Replace duplicated `query_sysvision`/`query_servicectl_units` request mechanics with one internal Unix HTTP GET helper that returns status plus a bounded body. Keep the existing 1 MiB response limit and five-second socket timeouts. URL-encode unit path segments; never interpolate an unchecked raw unit name.

The existing status operation may continue using sysvision in this task. `get_unit_snapshot` must use only the new servicectl endpoint and must not make a second sysvision request.

### Step 4: Add bounded JSON token extraction

The backend must remain libc-only. Extend the existing scanner into reusable routines that:

1. locate an exact key in the current object rather than a nested substring;
2. return a copied JSON string with standard escapes decoded;
3. return the exact source slice for an arbitrary JSON value;
4. iterate arrays and objects while respecting nesting and quoted escapes;
5. reject trailing malformed content.

Do not validate D-Bus signatures or typed JSON semantics in this module; that belongs to the broker. Do validate the HTTP response shape and required strings.

### Step 5: Implement snapshot allocation/freeing

Implement `servicectl_get_unit_snapshot` and `servicectl_free_unit_snapshot`, populate the operations table, and initialize output pointers only on success. Limit property count to 256 during decode and rely on the existing body limit for the encoded-size ceiling.

### Step 6: Verify backend behavior and linkage

Run:

```sh
meson test -C build -v test-systemd1-broker-backend-servicectl
ldd build/src/systemd1-broker/libsystemd1-broker-backend-servicectl.so
nm -D --defined-only build/src/systemd1-broker/libsystemd1-broker-backend-servicectl.so
git diff --check
```

Expected: tests PASS, linkage lists libc but no libsystemd, and only the ABI entry point is exported.

### Step 7: Commit the ABI/backend stage

Stage the ABI header, backend implementation, and backend test only. Include the AI disclosure in the commit message.

## Task 6: Add Broker Snapshot Validation And Atomic Cache Replacement

**Files:**

- Add: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker-metadata.c`
- Add: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker-metadata.h`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/meson.build`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker.c`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker.h`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/test/test-systemd1-broker.c`

### Step 1: Write failing cache/validation tests

Extend `TestBackendContext` with deep-copy snapshot support. Add tests for:

1. a valid snapshot atomically updates description, state, properties, and generation;
2. a later smaller successful snapshot removes omitted properties;
3. a later backend error leaves the complete previous snapshot intact;
4. duplicate interface/name keys reject the generation;
5. reserved keys (`Id`, `Names`, `Description`, `LoadState`, `ActiveState`, `SubState`, `Job`) are dropped without disturbing the remaining valid generation;
6. unsupported interfaces and `h` properties are ignored with the remaining accepted generation intact;
7. malformed names/signatures/JSON, type mismatch, null, integer overflow, more than 256 properties, and more than 1 MiB encoded data reject the generation;
8. a signature conflict with the manager-wide schema registry rejects the whole later generation, including on another unit;
9. duplicate property keys and duplicate member names across Unit and Service reject the generation;
10. backend-owned memory is freed once on every return path.

Use public inspection helpers rather than exposing struct fields directly:

```c
int systemd1_broker_manager_refresh_unit_snapshot(Systemd1BrokerManager *manager, const char *name, bool *ret_changed);
const Systemd1BrokerProperty* systemd1_broker_unit_find_property(Systemd1BrokerUnit *unit, const char *interface, const char *name);
size_t systemd1_broker_unit_n_properties(Systemd1BrokerUnit *unit);
uint64_t systemd1_broker_unit_metadata_generation(Systemd1BrokerUnit *unit);
```

### Step 2: Run the red broker test

Run:

```sh
meson test -C build -v test-systemd1-broker
```

Expected: compile failure because metadata types and refresh APIs do not exist.

### Step 3: Implement internal metadata ownership

Put property storage, schema keys, validation, comparison, and cleanup in the new metadata module. Use owned strings for interface, name, signature, and canonical JSON. Parse `value_json` with `sd_json_parse`; format accepted variants back with a stable compact format before comparing generations.

The manager owns a schema registry for the process lifetime. The unit owns its current property array and generation. Free unit properties in `systemd1_broker_unit_free` and registry entries in `systemd1_broker_manager_free`.

### Step 4: Validate signatures and JSON types recursively

Use `signature_is_valid()` plus `signature_element_length()` to require exactly one complete type. Implement recursive validation for basic types, arrays, structs, dictionary entries, and variants according to the design. Reject `h` at any nesting depth. Use exact range checks before accepting integer JSON values.

Separate validation from D-Bus serialization so malformed snapshots never begin a reply.

### Step 5: Implement prepare-then-commit refresh

`systemd1_broker_manager_refresh_unit_snapshot()` must:

1. call the backend once;
2. prepare all accepted properties and pending schema keys;
3. reject malformed generations without changing unit or registry;
4. compare canonical old/new property sets;
5. atomically replace description, state, properties, generation, and newly established schema keys;
6. free the backend response exactly once;
7. return the backend/validation error while preserving the old cache.

Keep `systemd1_broker_manager_refresh_unit_status()` for operation paths. Property paths use the rich snapshot API.

### Step 6: Run cache tests

Run:

```sh
meson test -C build -v test-systemd1-broker
```

Expected: all core cache and existing job/catalog tests PASS.

## Task 7: Add JSON-To-D-Bus Typed Serialization

**Files:**

- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker-metadata.c`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker-metadata.h`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/test/test-systemd1-broker.c`

### Step 1: Write failing serializer tests

Create sealed method-return messages and verify exact round trips for:

1. `s`, `o`, `g`, `b`;
2. every signed and unsigned integer width plus `d`;
3. `as`, nested arrays, byte arrays;
4. structs such as `(uo)`;
5. dictionaries represented as arrays of two-element arrays;
6. variants represented by signature/value objects;
7. the real `ExecStart` signature `a(sasbttttuii)`.

Also assert rejection of invalid object paths, invalid nested signatures, non-finite numbers, null, wrong array lengths, and out-of-range integers.

### Step 2: Run the red serializer tests

Run:

```sh
meson test -C build -v test-systemd1-broker
```

Expected: serializer tests fail.

### Step 3: Implement recursive appending

Add:

```c
int systemd1_broker_property_append_value(sd_bus_message *message, const Systemd1BrokerProperty *property);
int systemd1_broker_json_append_value(sd_bus_message *message, const char *signature, sd_json_variant *value);
```

Use `sd_bus_message_append_basic` for scalar values and explicit open/close container calls for arrays, structs, dictionary entries, and variants. Never call varargs append with JSON-derived types.

### Step 4: Run serializer and full broker tests

Run:

```sh
meson test -C build -v test-systemd1-broker
```

Expected: PASS.

## Task 8: Replace Placeholder GetAll With Dynamic Properties Dispatch

**Files:**

- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker-dbus.c`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/systemd1-broker/systemd1-broker-dbus.h`
- Modify: `/root/systemd-src/worktrees/systemd1-broker/src/test/test-systemd1-broker.c`

### Step 1: Write failing D-Bus integration tests

Configure the fake backend with a rich snapshot and call the real peer bus. Assert:

1. `Properties.GetAll("")` causes exactly one snapshot call and returns core Unit plus present dynamic Unit and Service properties;
2. `GetAll(Unit)` and `GetAll(Service)` each refresh once and filter by interface;
3. absent optional `MainPID`, `Result`, timestamps, and `PIDFile` are not in GetAll;
4. explicit absent `FragmentPath` returns `""` and `DropInPaths` returns an empty `as`;
5. another absent property returns `SD_BUS_ERROR_UNKNOWN_PROPERTY`;
6. `Set` returns property-read-only/not-supported;
7. one failed refresh after a good refresh serves the last valid generation;
8. introspection includes only current dynamic property names/signatures plus fixed methods/core properties;
9. a changed or removed dynamic property emits `PropertiesChanged` with the affected key in the invalidated string array;
10. existing Unit methods, Manager methods, Job objects, and job property signals still work.

Update old tests that currently require neutral Service placeholders: they must instead assert absence from GetAll.

### Step 2: Run the red integration test

Run:

```sh
meson test -C build -v test-systemd1-broker
```

Expected: current static vtables expose placeholder values and fail the new assertions.

### Step 3: Register a unit-path fallback callback

In `systemd1_broker_dbus_add_manager()`, register `sd_bus_add_fallback()` on `/org/freedesktop/systemd1/unit` before the fallback vtables. The callback must return zero for unrelated interfaces/members so existing vtable dispatch continues.

Handle only:

```text
org.freedesktop.DBus.Properties.Get
org.freedesktop.DBus.Properties.GetAll
org.freedesktop.DBus.Properties.Set
org.freedesktop.DBus.Introspectable.Introspect
```

Resolve the unit path through the existing manager lookup. Refresh at most once at callback entry. If refresh fails and a cached generation exists, continue with cache; if no rich cache exists, return core properties only.

### Step 4: Emit core and dynamic properties explicitly

Do not delegate GetAll to static vtables. Add explicit append helpers for broker-owned core values:

```text
Unit: Id, Names, Description, LoadState, ActiveState, SubState, Job
```

Only emit optional metadata when present. Do not emit `Following`, `NeedDaemonReload`, `InvocationID`, or any other placeholder as core metadata. Dynamic properties override old placeholder vtable getters because the callback handles Properties first. Keep `FragmentPath` and `DropInPaths` neutral values only for explicit Get compatibility.

For `GetAll("")`, emit a single `a{sv}` containing core Unit properties and dynamic values from both standard interfaces, matching systemctl's existing behavior expected by the design. For a named interface, filter strictly.

### Step 5: Generate dynamic introspection

Preserve existing fixed methods and signal definitions. Generate Unit and Service property XML from the current snapshot/schema with XML escaping and stable key ordering. Do not advertise stale registry keys that are absent from this unit's current snapshot.

### Step 6: Emit invalidation signals

After a successful refresh, compare old/new keys and canonical JSON. Group invalidated property names by interface and emit one `PropertiesChanged` signal per affected interface with an empty changed dictionary and the invalidated names. Keep existing state/job `PropertiesChanged` behavior unchanged.

### Step 7: Run integration and focused build tests

Run:

```sh
meson test -C build -v test-systemd1-broker
meson compile -C build systemd1-broker
git diff --check
```

Expected: PASS.

### Step 8: Commit broker core/D-Bus metadata

Inspect status, diff, and recent log. Stage only metadata module, core, D-Bus, Meson, and tests. Include the required AI disclosure.

## Task 9: End-To-End Verification And Deployment

**Files:**

- No source changes expected unless verification exposes a defect.

### Step 1: Run all focused source verification

In `/root/servicectl`:

```sh
go test . ./cmd/sys-dbusd ./internal/dbusactivation -count=1
bash scripts/test-install-paths.sh
git diff --check
```

In `/root/systemd-src/worktrees/systemd1-broker`:

```sh
meson test -C build -v test-systemd1-broker-backend-servicectl
meson test -C build -v test-systemd1-broker
meson compile -C build systemd1-broker systemd1-broker-backend-servicectl
ldd build/src/systemd1-broker/libsystemd1-broker-backend-servicectl.so
git diff --check
```

Expected: all PASS; backend remains independent of libsystemd.

### Step 2: Build and install the local artifacts

Build servicectl with the normal Go command already used for this workspace. Install the resulting binary and restart only the servicectl API supervisor. Confirm the new endpoint before touching the broker:

```sh
curl -sS --unix-socket /run/servicectl/servicectl.sock \
  http://localhost/v1/units/cpa-manager-plus.service
```

Confirm the JSON includes the real fragment path and positive PID from servicectl.

Build broker artifacts through Meson, install the executable, matching shared systemd library, and backend module to their existing local deployment paths, then restart `systemd1-broker.service` through servicectl.

### Step 3: Validate standard systemctl behavior

Run:

```sh
env SYSTEMD_IGNORE_CHROOT=1 systemctl show cpa-manager-plus.service
env SYSTEMD_IGNORE_CHROOT=1 systemctl show cpa-manager-plus.service \
  -p FragmentPath -p MainPID -p ExecMainPID -p Type -p WorkingDirectory -p User -p Group
env SYSTEMD_IGNORE_CHROOT=1 systemctl status cpa-manager-plus.service --no-pager
env SYSTEMD_IGNORE_CHROOT=1 systemctl cat cpa-manager-plus.service
busctl --system introspect org.freedesktop.systemd1 \
  /org/freedesktop/systemd1/unit/cpa_2dmanager_2dplus_2eservice
```

Expected:

1. `FragmentPath` is `/etc/systemd/system/cpa-manager-plus.service`.
2. `MainPID` and `ExecMainPID` equal servicectl's observed application PID.
3. `systemctl cat` prints the real unit file.
4. available Type/working-directory/user/group fields are real.
5. unsupported optional properties are absent from default show output rather than synthesized as zero/empty.
6. service state and existing start/stop/restart behavior still work.

### Step 4: Exercise stale-cache fallback

After one successful query, temporarily stop only the servicectl API, query the same properties, and confirm the broker serves the last valid generation. Restart the API immediately and confirm the next query refreshes to current values. Do not leave the control plane stopped.

### Step 5: Publish and inspect CI

Push only when explicitly requested or as part of the already established repository publication workflow. After pushing, inspect the new GitHub Actions runs and address actual failures. Do not assume local success proves distribution-specific `-Werror` or packaging jobs pass.

## Completion Criteria

The task is complete only when:

1. both repository worktrees are clean except for unrelated pre-existing changes;
2. all focused Go and Meson tests pass from fresh commands;
3. the backend shared library has no libsystemd dependency;
4. live `systemctl show/status/cat` expose real available metadata for `cpa-manager-plus.service`;
5. absent optional properties are omitted from GetAll;
6. stale-cache behavior is demonstrated;
7. commits contain the required AI disclosure and no unrelated files.
