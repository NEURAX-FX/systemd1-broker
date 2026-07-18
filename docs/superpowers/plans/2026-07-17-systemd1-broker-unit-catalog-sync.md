# systemd1 Broker Unit Catalog Synchronization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Synchronize every application service known to servicectl into the systemd1 broker so all `ListUnits*()` methods expose one complete, current catalog.

**Architecture:** Servicectl extends its existing Unix HTTP API with `GET /v1/units?all=1`. The standalone backend library converts that response into a size-versioned, backend-owned catalog snapshot. The broker validates and atomically reconciles snapshots, refreshes before list methods, and keeps the last complete catalog after transient refresh failures.

**Tech Stack:** Go `net/http` and JSON, C shared-library ABI, libc Unix sockets and JSON field parsing, sd-bus, Meson tests.

---

### Task 1: Servicectl All-Units API

**Files:**
- Modify: `/root/servicectl/servicectl_api.go`
- Modify: `/root/servicectl/servicectl_api_test.go`

- [ ] **Step 1: Write the failing API test**

Add a test that injects discovered units and property lists into a `servicectlPlaneServer`, requests `/v1/units?all=1`, and expects the sorted normalized union of discovered, enabled, runner, and effective names. Add a companion assertion that `/v1/units` still passes only `EffectiveUnits` to `collectSnapshots`.

```go
func TestServicectlAPIAllUnitsUsesCompleteCatalog(t *testing.T) {
    server := newServicectlPlaneServer(visionapi.ModeSystem, newServicectlEventHub())
    server.discoverUnits = func(Config) []string { return []string{"discovered", "duplicate.service"} }
    server.queryUnitLists = func(string) (visionapi.UnitListsResponse, error) {
        return visionapi.UnitListsResponse{
            EnabledUnits: []string{"enabled.service", "duplicate"},
            RunnerUnits: []string{"runner.service"},
            EffectiveUnits: []string{"effective.service"},
        }, nil
    }
    server.collectSnapshots = func(_ Config, units []string) visionapi.UnitsResponse {
        return visionapi.UnitsResponse{Units: []visionapi.UnitSnapshot{{Name: strings.Join(units, ",")}}}
    }
    // GET /v1/units?all=1 must produce discovered,duplicate,effective,enabled,runner.
}
```

- [ ] **Step 2: Verify RED**

Run: `go test . -run 'TestServicectlAPI(AllUnitsUsesCompleteCatalog|DefaultUnitsUsesEffectiveList)' -count=1`

Expected: FAIL because the server has no injectable discovery function and ignores `all=1`.

- [ ] **Step 3: Implement the minimal API extension**

Add `discoverUnits func(Config) []string` to `servicectlPlaneServer`, initialize it with `discoverSystemdUnits`, and select units as follows:

```go
units := lists.EffectiveUnits
if util.ExternalManagedValueEnabled(r.URL.Query().Get("all")) {
    units = append([]string{}, s.discoverUnits(s.cfg)...)
    units = append(units, lists.EnabledUnits...)
    units = append(units, lists.RunnerUnits...)
    units = append(units, lists.EffectiveUnits...)
}
util.WriteJSON(w, s.collectSnapshots(s.cfg, normalizeUnitListNames(units)))
```

- [ ] **Step 4: Verify GREEN**

Run: `go test . -run 'TestServicectlAPI|TestNormalizeUnitListNames' -count=1`

Expected: PASS.

### Task 2: Backend Catalog ABI And Decoder

**Files:**
- Modify: `src/systemd1-broker/systemd1-broker-backend-api.h`
- Modify: `src/systemd1-broker/systemd1-broker-backend-servicectl.c`
- Modify: `src/test/test-systemd1-broker-backend-servicectl.c`

- [ ] **Step 1: Write failing backend catalog tests**

Extend the Unix HTTP stub test to call `ops->list_units`, assert the request line is `GET /v1/units?all=1 HTTP/1.1`, and validate two decoded entries including descriptions and state mapping. Add malformed JSON, duplicate normalized id, invalid unit id, HTTP error, and empty catalog cases. Always call `ops->free_units` for successful snapshots.

- [ ] **Step 2: Verify RED**

Run: `meson test -C build -v test-systemd1-broker-backend-servicectl`

Expected: build failure because `Systemd1BrokerBackendOps` has no `list_units` or `free_units`.

- [ ] **Step 3: Extend the ABI**

Append these declarations without reordering existing fields:

```c
typedef struct Systemd1BrokerBackendUnit {
        size_t size;
        const char *id;
        const char *description;
        Systemd1BrokerBackendState state;
} Systemd1BrokerBackendUnit;

int (*list_units)(void *userdata, Systemd1BrokerBackendUnit **ret_units, size_t *ret_n_units);
void (*free_units)(void *userdata, Systemd1BrokerBackendUnit *units, size_t n_units);
```

- [ ] **Step 4: Implement structured catalog decoding**

Reuse the bounded Unix HTTP transport and state mapping. Add a request helper for `/v1/units?all=1`, decode the `units` JSON array, normalize suffixless names to `.service`, reject invalid/duplicate ids, allocate copied entries, and implement one cleanup callback that frees every id, description, and the array. Do not add libsystemd dependencies or invoke the CLI.

- [ ] **Step 5: Verify GREEN**

Run: `meson test -C build -v test-systemd1-broker-backend-servicectl`

Expected: PASS, including ABI export and malformed response tests.

### Task 3: Atomic Broker Catalog Reconciliation

**Files:**
- Modify: `src/systemd1-broker/systemd1-broker.c`
- Modify: `src/systemd1-broker/systemd1-broker.h`
- Modify: `src/test/test-systemd1-broker.c`

- [ ] **Step 1: Extend the fake backend and write failing reconciliation tests**

Add fake catalog snapshots and call counters to `TestBackendContext`. Test initial add, description/state update, disappearance removal, active-job removal deferral, on-demand unit retention, duplicate rejection, and refresh error retention.

```c
ASSERT_OK(systemd1_broker_manager_sync_units(manager));
ASSERT_NOT_NULL(systemd1_broker_manager_get_unit(manager, "alpha.service"));
ASSERT_STREQ(systemd1_broker_unit_active_state(alpha), "active");
```

- [ ] **Step 2: Verify RED**

Run: `meson test -C build -v test-systemd1-broker`

Expected: build failure because `systemd1_broker_manager_sync_units` is absent.

- [ ] **Step 3: Implement atomic reconciliation**

Add `catalog_generation` to units and manager. Validate the full backend result before mutation. Prepare copied names/descriptions and required manager capacity first, then increment generation, update/add returned units, and remove older catalog-managed units without jobs. Keep generation-zero on-demand units. Call `free_units` exactly once after each successful `list_units` return, including validation failures.

- [ ] **Step 4: Verify GREEN**

Run: `meson test -C build -v test-systemd1-broker`

Expected: PASS for all reconciliation and existing job tests.

### Task 4: ListUnits Refresh Integration

**Files:**
- Modify: `src/systemd1-broker/systemd1-broker-dbus.c`
- Modify: `src/systemd1-broker/systemd1-broker-main.c`
- Modify: `src/test/test-systemd1-broker.c`

- [ ] **Step 1: Write failing refresh-before-filter tests**

For each Manager list method, change the fake snapshot between calls and assert the response reflects the new snapshot before state/name/pattern filtering. Test that a later `list_units` error serves the previous snapshot.

- [ ] **Step 2: Verify RED**

Run: `meson test -C build -v test-systemd1-broker`

Expected: FAIL because list methods only read current manager arrays.

- [ ] **Step 3: Synchronize in the D-Bus list handlers**

Call `systemd1_broker_manager_sync_units()` before collecting infos for `ListUnits`, `ListUnitsFiltered`, `ListUnitsByPatterns`, and `ListUnitsByNames`. Treat a refresh failure after a prior successful generation as stale-cache success; propagate failure if no successful generation exists.

- [ ] **Step 4: Add fatal initial synchronization**

After backend loading and before exporting Manager objects, call `systemd1_broker_manager_sync_units()`. Remove the production `alpha.service` seed. Keep test-only units inside test setup.

- [ ] **Step 5: Verify GREEN**

Run: `meson test -C build -v test-systemd1-broker`

Expected: PASS, including executable smoke tests without a static alpha seed.

### Task 5: Cross-Repository Verification And Deployment

**Files:**
- No new source files unless verification exposes a regression.

- [ ] **Step 1: Run focused suites**

Run:

```sh
go test . ./cmd/sys-dbusd ./internal/dbusactivation -count=1
bash scripts/test-install-paths.sh
meson test -C build -v test-systemd1-broker-backend-servicectl
meson test -C build -v test-systemd1-broker
```

Expected: all PASS.

- [ ] **Step 2: Verify standalone linkage**

Run: `ldd build/src/systemd1-broker/libsystemd1-broker-backend-servicectl.so`

Expected: libc/loader only; no libsystemd or private systemd shared object.

- [ ] **Step 3: Build and deploy both sides**

Build/install current servicectl and sys-dbusd binaries, then build the broker targets with Meson and install the broker binary, backend library, and matching private shared library. Restart the servicectl API, sys-dbusd, and systemd1-broker through the currently active servicectl/s6 paths.

- [ ] **Step 4: Compare live catalogs**

Collect `servicectl list --json` and `systemctl list-units --all --no-legend --plain`, normalize `.service` names, and verify the broker set contains the complete servicectl application set. Confirm default `systemctl list-units` excludes inactive entries while `--all` includes them.

- [ ] **Step 5: Verify activation remains healthy**

Run a cold `localectl`, inspect `ListJobs`, and confirm `sys-dbusd.service` and `systemd1-broker.service` remain active with no stale broker jobs.
