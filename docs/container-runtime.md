# Container Runtime — Foundation (Ayush)

Foundation module: container metadata + lifecycle only. No namespaces, no
cgroups, no DB logic, no Bridge logic.

## Files

| File | Purpose |
|---|---|
| `include/container/container.h` | Public API: `nexus_container_t`, `nexus_state_t`, error codes, `container_create/start/stop/delete/status/get` |
| `src/container/container.c` | In-memory registry (64 slots) + lifecycle transitions + validation |
| `include/container/process.h` / `src/container/process.c` | Placeholder child process: Linux `fork/exec(sleep)/waitpid`; Windows `CreateProcess(ping)` shim for dev machines without WSL |
| `tests/container/container_test.c` | Lifecycle demo + error-path checks: create → start → status → stop → delete |
| `Makefile` | `make` / `make test` / `make clean` (Linux `make`, Windows `mingw32-make`) |

## Lifecycle

```
CREATE (container_create) → RUNNING (container_start) →
STOPPED (container_stop) → DELETED (container_delete, tombstone)
```

- `create` validates name `[A-Za-z0-9_-]{1,63}`, cpu 0–100, stores the
  *intended* cgroup path `/sys/fs/cgroup/nexusdb/<name>` (recorded only —
  not created until the cgroups phase) and the configured DB-engine path.
- `start` allowed from CREATED/STOPPED; spawns the placeholder child and
  records its PID. Second `start` → `NEXUS_ERR_STATE`.
- `stop` sends SIGTERM, waits ~2s reaping via `waitpid`, then SIGKILL +
  blocking reap. No zombies. Non-RUNNING → `NEXUS_ERR_STATE`.
- `delete` refuses RUNNING containers (stop first — no orphans). Keeps a
  DELETED tombstone so status-after-delete is observable; re-creating the
  same name reuses the slot.

## Integration boundaries (do not cross)

- DB engine (Digvijay): only the `db_engine` path string is stored. The
  placeholder child will be replaced by fork/exec of that binary in the
  DB-launch phase. No SQL/pages/LRU/WAL here.
- OS–DB Bridge (Rehan): reads state via `container_get`/`container_status`
  (name, id, pid, state, `memory_bytes`, `cpu_percent`, `cgroup_path`).
  Adaptation policy lives in the Bridge, never here.

## Build / test (Ubuntu/WSL2)

```sh
make test
```

Expected: `ALL TESTS PASSED`. On a Windows dev machine without WSL, use
`mingw32-make test` — same assertions, placeholder child is `ping.exe`.

## Limitations (to be fixed in later phases)

- Registry is in-process memory: no persistence across CLI invocations
  (a file-backed state store comes with the CLI/runtime-coordinator work).
- No namespaces / cgroups / CPU+memory enforcement yet.
- No async SIGCHLD reaper: a crashed child is reaped on `stop`/`delete`,
  not immediately.
- Single-threaded registry, no locking.
- Requires pthreads? No. Requires root? No — foundation needs none.
