# Container Runtime — Complete Specification (Ayush: Systems Engineer)

Custom container engine built from raw Linux syscalls (no Docker, no `libcontainer`). Manages database process isolation, resource enforcement via cgroups v2, and provides real-time telemetry signals to the OS–DB Bridge.

## Architecture & Files

| File | Purpose |
|---|---|
| `include/container/container.h` | Public API: `nexus_container_t`, `nexus_state_t`, lifecycle functions, `container_metrics` |
| `src/container/container.c` | In-memory registry (64 slots) + lifecycle transitions + validation |
| `include/container/cgroup.h` / `src/container/cgroup.c` | Linux cgroups v2 controller (`memory.max`, `memory.current`, `cpu.max`, `cgroup.procs`) & Bridge telemetry |
| `include/container/namespace.h` / `src/container/namespace.c` | Linux namespaces isolation (`CLONE_NEWPID`, `CLONE_NEWNS`, `CLONE_NEWNET`, `CLONE_NEWUTS`, `CLONE_NEWIPC`) |
| `include/container/process.h` / `src/container/process.c` | Child process launcher & supervision: Linux `fork/exec/waitpid`; Windows `CreateProcess` shim |
| `tests/container/container_test.c` | Comprehensive test suite: lifecycle, telemetry metrics, state validation, namespaces |
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
- **start**: Permitted from `CREATED` or `STOPPED`. Spawns child process, applies namespaces, attaches PID to `cgroup.procs`, moves state to `RUNNING`. Second `start` returns `NEXUS_ERR_STATE`.
- **metrics**: Queries real-time memory usage (`memory.current`), memory limit (`memory.max`), CPU time, and computes memory pressure ratio for the OS–DB Bridge.
- **stop**: Sends `SIGTERM`, grace period with `waitpid`, `SIGKILL` fallback. Prevents zombie processes. Moves state to `STOPPED`.
- **delete**: Refuses `RUNNING` containers (must be stopped first to prevent orphaned processes). Destroys container cgroup. Preserves tombstone for observable post-deletion status; reusing the same name recycles the slot cleanly.

## Integration Boundaries

- **Database Engine (Digvijay)**: The container runtime launches the configured `db_engine` executable. Storage, page format, buffer pool, and recovery reside entirely in the DB engine module.
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
