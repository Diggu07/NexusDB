# Container Runtime — Complete Specification (Ayush: Systems Engineer)

Custom container engine built from raw Linux syscalls (no Docker, no `libcontainer`). Manages database process isolation, resource enforcement via cgroups v2, and provides real-time telemetry signals to the OS–DB Bridge.

## Architecture & Files

| File | Purpose |
|---|---|
| `include/container/container.h` | Public API: `nexus_container_t`, `nexus_state_t`, lifecycle functions, `container_metrics` |
| `src/container/container.c` | In-memory registry (64 slots) + lifecycle transitions + validation |
| `include/container/cgroup.h` / `src/container/cgroup.c` | Linux cgroups v2 controller (`memory.max`, `memory.current`, `cpu.max`, `cgroup.procs`) & Bridge telemetry |
| `include/container/namespace.h` / `src/container/namespace.c` | Linux namespaces isolation helpers (`CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWNET`, `CLONE_NEWUTS`, `CLONE_NEWIPC`). `namespace_clone_flags()` maps a config to clone flags and `namespace_configure_child()` applies the child-side setup (hostname, private mounts); both are **enforced by the lifecycle** via `container_create_ex()` + the gated clone path |
| `include/container/process.h` / `src/container/process.c` | Child process launcher & supervision: Linux `fork/exec/waitpid`; Windows `CreateProcess` shim |
| `src/db/engine/db_engine.cpp` | The real DB-engine executable (`db_engine`) the container launches as its workload: opens the full `Database` (PageStorage + **WAL** + BufferPool + Catalog/QueryExecutor), running WAL recovery on startup, stays alive until `SIGTERM`/`SIGINT`, then flushes (WAL-ordered) and closes. No SQL, no network listener, no client/server protocol |
| `include/manager/manager.h`, `src/manager/manager.cpp` | R3 Manager: routes container lifecycle to the runtime (with persisted metadata + `container_adopt` rehydration) and database CRUD to `Database`/`QueryExecutor` |
| `include/manager/router.h`, `src/manager/router.cpp` | R3 Router: validates argv and dispatches to the Manager; owns only syntax/hex/number validation |
| `src/cli/nexusdb_cli.cpp` | R3 CLI executable (`nexusdb`): thin front end — parse via Router, print result, return a stable exit code |
| `tests/container/container_test.c` | Comprehensive test suite: lifecycle, telemetry metrics, state validation, namespaces, configured DB-engine launch |
| `tests/container/db_engine_test.c` | `db_engine` executable contract test (direct-exec): stays alive, clean `SIGTERM` shutdown, usage exit 2, storage-failure exit 3 |
| `tests/container/namespace_test.c` | Linux namespace isolation test (env-limited without `CAP_SYS_ADMIN`) |
| `tests/container/cpu_enforce_test.c` | Linux cgroup v2 CPU enforcement test (`cpu.max` throttling, `cpu.stat` evidence; env-limited without a delegated cgroup) |
| `tests/container/mem_enforce_test.c` | Linux cgroup v2 memory enforcement test (`memory.max` OOM kill vs within-limit vs alloc failure; env-limited without a delegated cgroup) |
| `tests/container/cpu_workload.c` / `mem_workload.c` | Bounded CPU / memory-pressure workload helpers driven by the enforcement tests |
| `scripts/setup.sh` | Environment verification & runtime directory initialization |
| `scripts/setup_cgroups.sh` | cgroups v2 hierarchy and controller delegation setup |
| `scripts/cleanup.sh` | Process teardown, cgroup cleanup, and test artefact removal |
| `Makefile` / `CMakeLists.txt` | Dual build support: Linux (`make` / `cmake`) and Windows (`mingw32-make`) |

## Lifecycle State Machine

```
CREATE (container_create) ──────> RUNNING (container_start)
         │                               │
         │                               ▼
         │                        STOPPED (container_stop)
         │                               │
         ▼                               ▼
DELETED (container_delete, tombstone pattern)
```

- **create**: Validates name `[A-Za-z0-9_-]{1,63}`, cpu `0–100`. Creates container subcgroup `/sys/fs/cgroup/nexusdb/<name>`, configures `memory.max` and `cpu.max`.
- **start**: Permitted from `CREATED` or `STOPPED`. Spawns the **workload** — the configured `db_engine` executable (`<db_engine> --data-dir <root>/<name>`, with `<root>` from `$NEXUSDB_DATA_ROOT`, default `./nexusdb_data`), or the built-in placeholder (`sleep 3600` / `ping`) when no engine is configured — **blocked before `exec`** inside any namespaces requested at `container_create_ex()` time (`clone()` with the mapped `CLONE_NEW*` flags; the child-side hook configures hostname/private mounts **before** releasing the gate), attaches its PID to `cgroup.procs`, then releases the child to run. An executable startup failure (missing/unexecutable engine, failed `exec`) aborts cleanly: no zombie, container stays in its prior state. A namespace request **or a configured `db_engine`** on Windows returns `NEXUS_ERR_UNSUPPORTED` (never a false RUNNING), and a child-side setup failure aborts the still-blocked child and returns `NEXUS_ERR_PROCESS`. Second `start` returns `NEXUS_ERR_STATE`. Namespace isolation requires `CAP_SYS_ADMIN`; the product does **not** create user namespaces or map UID/GID (see `namespace.h`).
- **metrics**: Queries real-time memory usage (`memory.current`), memory limit (`memory.max`), CPU time, and computes memory pressure ratio for the OS–DB Bridge.
- **stop**: Sends `SIGTERM`, grace period with `waitpid`, `SIGKILL` fallback. Prevents zombie processes. Moves state to `STOPPED`.
- **delete**: Refuses `RUNNING` containers (must be stopped first to prevent orphaned processes). Destroys container cgroup. Preserves tombstone for observable post-deletion status; reusing the same name recycles the slot cleanly.

## Integration Boundaries

- **Database Engine (Digvijay)**: When a container is created with a non-empty `db_engine` path, the runtime launches it as the container workload, invoked as `db_engine --data-dir <root>/<name>` and gated so it cannot run outside its cgroup or requested namespaces. The executable (`src/db/engine/db_engine.cpp`) opens the full `Database` (`Database::Open(<dir>/nexusdb.db, <dir>/nexusdb.wal, pool)`): it opens and validates the WAL, replays it into storage **before** the buffer pool exists, checkpoints, then opens the Catalog/QueryExecutor; on a fresh database it reserves header page 0 and creates a catalog at page 1. It then idles until `SIGTERM`/`SIGINT`, when `Database::Close()` flushes all dirty pages **WAL-ordered**, resets/closes the log, and closes storage. An abrupt termination leaves the WAL for the next Open. When no engine is configured the built-in placeholder runs instead. There is **no SQL, no network listener, and no client/server protocol**; storage, page format, buffer pool, WAL, and recovery reside in the DB engine module. The gated protocol guarantees `exec` succeeded, not that the engine finished its own initialization (post-exec init failure surfaces in the engine's exit code).
- **Manager / Router / CLI (Digvijay, R3)**: The `nexusdb` CLI is a one-shot, per-command process. The Router parses/validates argv; the Manager coordinates (a) container lifecycle — routed through this runtime, but with each container's metadata **persisted** to `<data_root>/.nexus/containers/<name>.meta` and re-adopted (`container_adopt`, additive) into the process-local registry before each operation — and (b) database operations, opened directly through `Database` on `<data_root>/<name>`. Because there is no IPC, database commands **refuse to run while the container is RUNNING** (the engine holds the database file open); stop the container first. `container start` re-spawns the configured engine from the persisted config; `container stop` signals the recorded pid and returns it to STOPPED. The data directory is never deleted on stop or delete.
- **OS–DB Bridge (Rehan)**: Queries resource metrics via `container_metrics(name, &metrics)`. The bridge evaluates pressure thresholds ($>85\%$ shrink, $<60\%$ grow) and triggers buffer pool resizing in the DB engine.

## Build and Verification

### Linux (Ubuntu 22.04 / WSL2)
```bash
sudo ./scripts/setup.sh
sudo ./scripts/setup_cgroups.sh
make test
```

### Windows Dev Environment (MinGW)
```cmd
mingw32-make test
```
All tests pass cleanly with `-Wall -Wextra -Werror`.

### db_engine executable contract

The `db_engine` target is a minimal, long-running database-engine process:

```
db_engine [--data-dir <dir>] [--buffer-pool <frames>]
```

- `--data-dir <dir>` — directory holding `nexusdb.db` (created if missing);
  default `$NEXUSDB_DATA_DIR` or `./nexusdb_data`.
- `--buffer-pool <n>` — buffer-pool capacity in pages (default 64, min 1).

Startup opens `PageStorage` on `<dir>/nexusdb.db`, creates a `BufferPool` over
it, allocates a bootstrap page when the file is empty, and fetches it through the
pool. It then blocks (no busy loop) until `SIGTERM`/`SIGINT`, when it
`FlushAll()`s and closes. Exit codes: `0` clean shutdown, `2` usage error,
`3` storage failure, `4` buffer-pool failure, `5` shutdown-flush failure.

`tests/container/db_engine_test.c` pins this contract by launching the binary
directly, and `tests/container/container_test.c` launches it through the
container. The build passes the engine's path to both tests via
`$NEXUSDB_DB_ENGINE`; if the binary is unavailable the launch checks are reported
**ENV-LIMITED**, never assumed.

### Namespace isolation tests

Namespace isolation is verified by `tests/container/namespace_test.c`, which
checks that a gated child is genuinely in new mount/PID/net/UTS/IPC namespaces
(via `/proc/<pid>/ns/*`) with the workload as PID 1 (`NSpid` depth 2). Creating
these namespaces requires `CAP_SYS_ADMIN`; without it the run is reported
**ENV-LIMITED** (denial/failure semantics are still asserted, never counted as
passing). To exercise the real isolation path unprivileged, run it inside a user
namespace that grants the capability, e.g.:

```bash
unshare -U --map-root-user ./build-linux-clean/nexus_namespace_test
```

Note: cgroup v2 migration into a non-initial user namespace is typically denied,
so full namespace-plus-cgroup lifecycle runs require either root or a delegated
cgroup hierarchy.

### CPU and memory enforcement tests (P4)

`tests/container/cpu_enforce_test.c` and `tests/container/mem_enforce_test.c`
prove that `cpu.max` and `memory.max` are actually **enforced** by the kernel,
not merely written. Both require a real, writable cgroup v2 hierarchy (a
delegated systemd user session with `$NEXUS_CGROUP_ROOT` set); where that is
unavailable they print **ENV-LIMITED** and exit 0 rather than claim a result.

- **CPU** — `nexus_cpu_workload` (a wall-clock-bounded burner) is attached to a
  disposable cgroup *before* it execs and run twice: unrestricted, then throttled
  to 20% of one CPU. Kernel `cpu.stat` is read before/after each run. The
  restricted run must show `nr_throttled >= 1` and `throttled_usec > 0`, with
  `usage_usec` materially below the unrestricted run; the unrestricted run must
  show `nr_throttled == 0`. Example evidence: unrestricted `usage_usec=800091`
  vs throttled `usage_usec=183447, nr_throttled=9, throttled_usec=660400`.
- **Memory** — `nexus_mem_workload` is run past `memory.max` with
  `memory.swap.max = 0`, forcing a cgroup OOM kill: the process is killed by
  `SIGKILL` and `memory.events` `oom_kill`/`max` increment. The same test
  distinguishes the three outcomes — kernel enforcement (`SIGKILL` + `oom_kill`),
  a within-limit run (clean exit 0, no OOM), and a plain allocation failure
  (exit 4, no OOM).

Delegated run (no sudo, throttle + OOM enforcement active):

```bash
XDG_RUNTIME_DIR=/run/user/1000 systemd-run --user --scope --quiet \
  env NEXUS_CGROUP_ROOT=/sys/fs/cgroup/user.slice/user-1000.slice/user@1000.service/nexusdb \
  ctest --test-dir build-linux-clean --output-on-failure
```
