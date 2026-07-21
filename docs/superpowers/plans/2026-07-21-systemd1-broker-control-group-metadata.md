# systemd1 Broker ControlGroup Metadata Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the authoritative sys-cgroupd unit path as the standard `org.freedesktop.systemd1.Unit.ControlGroup` property.

**Architecture:** The servicectl per-unit detail endpoint performs one bounded `get-unit` request against sys-cgroupd, normalizes a tracked path below `/sys/fs/cgroup` to systemd's hierarchy-relative form, and stores it on the existing `UnitSnapshot`. `buildSystemdProperties` emits `ControlGroup` through the existing typed metadata array. The C backend and broker already pass arbitrary validated Unit properties, so they require no cgroup-specific code.

**Tech Stack:** Go 1.24, servicectl Unix control APIs, sys-cgroupd protocol, typed JSON metadata, existing C backend and sd-bus broker.

---

### Task 1: Finish The HTTP Framing Prerequisite

**Files:**
- Modify: `src/systemd1-broker/systemd1-broker-backend-servicectl.c`
- Test: `src/test/test-systemd1-broker-backend-servicectl.c`

- [ ] **Step 1: Run the chunked-response regression tests**

Run:

```sh
meson test -C build -v test-systemd1-broker-backend-servicectl
```

Expected: PASS, including chunked catalog, detail, and sysvision responses.

- [ ] **Step 2: Verify the backend remains standalone**

Run:

```sh
ldd build/src/systemd1-broker/libsystemd1-broker-backend-servicectl.so
nm -D --defined-only build/src/systemd1-broker/libsystemd1-broker-backend-servicectl.so
```

Expected: only libc and the dynamic loader are linked; only `systemd1_broker_backend_get_ops` is exported.

- [ ] **Step 3: Commit the framing fix**

```sh
git add src/systemd1-broker/systemd1-broker-backend-servicectl.c \
        src/test/test-systemd1-broker-backend-servicectl.c
git commit -m "systemd1-broker: decode chunked backend responses" \
  -m "Co-developed-by: OpenCode <noreply@opencode.ai>"
```

### Task 2: Add ControlGroup To The Servicectl Snapshot

**Files:**
- Modify: `/root/servicectl/internal/visionapi/types.go`
- Modify: `/root/servicectl/servicectl_api.go`
- Test: `/root/servicectl/servicectl_api_test.go`

- [ ] **Step 1: Write failing normalization tests**

Add table-driven tests for the path conversion helper:

```go
func TestSystemdControlGroupPath(t *testing.T) {
    tests := []struct {
        path string
        want string
        ok   bool
    }{
        {path: "/sys/fs/cgroup/servicectl.slice/system/demo", want: "/servicectl.slice/system/demo", ok: true},
        {path: "/sys/fs/cgroup", want: "/", ok: true},
        {path: "/sys/fs/cgroup/servicectl.slice/../system/demo", want: "/system/demo", ok: true},
        {path: "/tmp/demo", ok: false},
        {path: "relative/demo", ok: false},
        {path: "", ok: false},
    }
    for _, test := range tests {
        got, ok := systemdControlGroupPath(test.path)
        if got != test.want || ok != test.ok {
            t.Fatalf("systemdControlGroupPath(%q) = %q, %v; want %q, %v", test.path, got, ok, test.want, test.ok)
        }
    }
}
```

- [ ] **Step 2: Write failing metadata inclusion and omission tests**

Extend `TestBuildSystemdPropertiesIncludesAvailableMetadata` with:

```go
snapshot.CgroupPath = "/servicectl.slice/system/demo"
assertSystemdPropertyJSON(
    t,
    properties,
    "org.freedesktop.systemd1.Unit",
    "ControlGroup",
    "s",
    `"/servicectl.slice/system/demo"`,
)
```

Extend `TestBuildSystemdPropertiesOmitsUnavailableMetadata` to assert that an empty `CgroupPath` does not publish `ControlGroup`.

- [ ] **Step 3: Write failing cgroup lookup tests**

Add dependency-injected lookup tests:

```go
func TestResolveUnitControlGroup(t *testing.T) {
    cfg := Config{Mode: visionapi.ModeSystem}
    calls := 0
    lookup := func(ctx context.Context, got Config, unit string) (cgrouptrack.UnitStatus, error) {
        calls++
        if got.Mode != visionapi.ModeSystem || unit != "demo.service" {
            t.Fatalf("lookup input = mode %q unit %q", got.Mode, unit)
        }
        return cgrouptrack.UnitStatus{Path: "/sys/fs/cgroup/servicectl.slice/system/demo"}, nil
    }
    got := resolveUnitControlGroup(context.Background(), cfg, "demo.service", lookup)
    if got != "/servicectl.slice/system/demo" || calls != 1 {
        t.Fatalf("control group = %q, calls = %d", got, calls)
    }
}
```

Add table cases where lookup returns `os.ErrNotExist`, another error, and `/tmp/demo`; `resolveUnitControlGroup` must return an empty string for all three.

- [ ] **Step 4: Run the red tests**

Run:

```sh
go test . -run 'Test(SystemdControlGroupPath|ResolveUnitControlGroup|BuildSystemdProperties)' -count=1
```

Expected: FAIL because `CgroupPath`, `systemdControlGroupPath`, and `resolveUnitControlGroup` do not exist yet.

- [ ] **Step 5: Add the snapshot field**

Add to `visionapi.UnitSnapshot`:

```go
CgroupPath string `json:"cgroup_path,omitempty"`
```

- [ ] **Step 6: Implement strict path normalization**

Add to `servicectl_api.go`:

```go
func systemdControlGroupPath(path string) (string, bool) {
    const cgroupRoot = "/sys/fs/cgroup"

    clean := filepath.Clean(strings.TrimSpace(path))
    if !filepath.IsAbs(clean) {
        return "", false
    }
    relative, err := filepath.Rel(cgroupRoot, clean)
    if err != nil || relative == ".." || strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
        return "", false
    }
    if relative == "." {
        return "/", true
    }
    return "/" + filepath.ToSlash(relative), true
}
```

- [ ] **Step 7: Add the bounded default lookup**

Define:

```go
type unitCgroupLookup func(context.Context, Config, string) (cgrouptrack.UnitStatus, error)

func queryUnitDetailCgroup(ctx context.Context, cfg Config, unit string) (cgrouptrack.UnitStatus, error) {
    uid := uint32(0)
    if strings.EqualFold(cfg.Mode, visionapi.ModeUser) {
        uid = uint32(os.Geteuid())
    }
    return queryStatusCgroupUnit(ctx, cgroupdSocketPath, cfg.Mode, uid, unit, doCgroupRequest)
}

func resolveUnitControlGroup(ctx context.Context, cfg Config, unit string, lookup unitCgroupLookup) string {
    if lookup == nil {
        return ""
    }
    status, err := lookup(ctx, cfg, unit)
    if err != nil {
        return ""
    }
    path, ok := systemdControlGroupPath(status.Path)
    if !ok {
        return ""
    }
    return path
}
```

Keep `buildUnitDetailResponse` as the production wrapper and add an injectable helper:

```go
func buildUnitDetailResponse(cfg Config, unitName string, lists visionapi.UnitListsResponse) (visionapi.UnitDetailResponse, error) {
    return buildUnitDetailResponseWithCgroup(cfg, unitName, lists, queryUnitDetailCgroup)
}

func buildUnitDetailResponseWithCgroup(
    cfg Config,
    unitName string,
    lists visionapi.UnitListsResponse,
    lookup unitCgroupLookup,
) (visionapi.UnitDetailResponse, error) {
    unitSnapshotConfigMu.Lock()
    defer unitSnapshotConfigMu.Unlock()
    previous := config
    config = cfg
    defer func() { config = previous }()

    unit, err := parseSystemdUnit(unitName)
    if err != nil {
        if isStatusUnitNotFoundError(err) {
            return visionapi.UnitDetailResponse{}, fmt.Errorf("%w: %v", errUnitDetailNotFound, err)
        }
        return visionapi.UnitDetailResponse{}, err
    }
    socketUnit, err := parseOptionalSocketUnit(unit.Name)
    if err != nil {
        return visionapi.UnitDetailResponse{}, err
    }
    snapshot := buildUnitSnapshotFromParsed(cfg, unitName, unit, socketUnit)
    ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
    defer cancel()
    snapshot.CgroupPath = resolveUnitControlGroup(ctx, cfg, unitName, lookup)
    properties, err := buildSystemdProperties(unit, snapshot, lists)
    if err != nil {
        return visionapi.UnitDetailResponse{}, err
    }
    return visionapi.UnitDetailResponse{Unit: snapshot, SystemdProperties: properties}, nil
}
```

Lookup failures are optional-metadata failures and must not fail the endpoint.

- [ ] **Step 8: Emit the standard property**

Inside `buildSystemdProperties`, after `FragmentPath`:

```go
if snapshot.CgroupPath != "" {
    if err := appendProperty(unitInterface, "ControlGroup", "s", snapshot.CgroupPath); err != nil {
        return nil, err
    }
}
```

- [ ] **Step 9: Run focused and full tests**

Run:

```sh
gofmt -w servicectl_api.go servicectl_api_test.go internal/visionapi/types.go
go test . -run 'Test(SystemdControlGroupPath|ResolveUnitControlGroup|BuildSystemdProperties)' -count=1
go test . ./cmd/sys-dbusd ./internal/dbusactivation -count=1
bash scripts/test-install-paths.sh
```

Expected: all commands PASS.

- [ ] **Step 10: Commit servicectl cgroup metadata**

```sh
git add internal/visionapi/types.go servicectl_api.go servicectl_api_test.go
git commit -m "feat: expose systemd control group metadata"
```

### Task 3: Verify And Deploy The Complete Path

**Files:**
- Deploy: `/usr/local/bin/servicectl`
- Deploy: `/usr/local/lib/systemd1-broker/libsystemd1-broker-backend-servicectl.so`

- [ ] **Step 1: Build both artifacts**

Run:

```sh
go build -o /tmp/opencode/servicectl-control-group .
meson compile -C build systemd1-broker systemd1-broker-backend-servicectl
```

Expected: both builds PASS.

- [ ] **Step 2: Deploy and restart the API and broker**

Run the existing verified deployment sequence:

```sh
install -m 0755 /tmp/opencode/servicectl-control-group /usr/local/bin/servicectl
s6-svc -r /run/s6/state:s6-rc-init:*/servicedirs/servicectl-api
install -m 0644 build/src/systemd1-broker/libsystemd1-broker-backend-servicectl.so \
  /usr/local/lib/systemd1-broker/libsystemd1-broker-backend-servicectl.so
/usr/local/bin/servicectl restart systemd1-broker.service
```

- [ ] **Step 3: Verify the raw typed API**

Run:

```sh
curl -sS --unix-socket /run/servicectl/servicectl.sock \
  http://localhost/v1/units/cpa-manager-plus.service
```

Expected: `unit.cgroup_path` is `/servicectl.slice/system/cpa-manager-plus` and `systemd_properties` contains `Unit.ControlGroup` with signature `s` and the same value.

- [ ] **Step 4: Verify standard systemd clients**

Run:

```sh
env SYSTEMD_IGNORE_CHROOT=1 systemctl show cpa-manager-plus.service \
  -p ControlGroup -p FragmentPath -p MainPID
busctl --system introspect org.freedesktop.systemd1 \
  /org/freedesktop/systemd1/unit/cpa_2dmanager_2dplus_2eservice
```

Expected: `ControlGroup=/servicectl.slice/system/cpa-manager-plus`; introspection includes `ControlGroup` as a read-only string property.

- [ ] **Step 5: Run final regression suites**

Run:

```sh
go test . ./cmd/sys-dbusd ./internal/dbusactivation -count=1
meson test -C build -v test-systemd1-broker
meson test -C build -v test-systemd1-broker-backend-servicectl
git diff --check
```

Expected: all tests PASS and both repositories are clean except for intentional commits ahead of their remotes.
