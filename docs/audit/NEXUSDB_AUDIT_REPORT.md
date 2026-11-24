# NexusDB — Forensic Audit Report

> **Status:** This is a historical forensic snapshot dated 2026-10-29. Sections below are preserved as audit evidence and are intentionally not rewritten. Current implementation deltas as of 2026-11-24 are summarized in the remediation-status addendum after the findings register (Section E).

**Project:** NexusDB — minimal container runtime + database engine (C/C++, Ubuntu/WSL2 target)
**Audit date:** 2026-10-29
**Repository:** `/mnt/d/NexusDB`
**Build tree audited:** `build-linux-clean` (CMake primary; root `Makefile` parallel path)
**Scope:** Read-only, evidence-based. No source, data, cgroup, or shell-configuration changes were made. All conclusions are drawn from source code, headers, tests, README/docs, CLI help output, live binary execution against disposable temp data roots, and `ctest` results.

**Working-tree note:** The tree was already dirty before this audit (38 modified tracked files + 6 untracked, pre-existing from prior development). This audit did not introduce, revert, or selectively keep any of those changes; it audits the tree as it stands. The only untracked files this audit added are `docs/audit/` (this report) — nothing else was written to the repo.

---

## A. Executive Summary

### Overall maturity

NexusDB is a ground-up, phase-built "OS↔database feedback loop" experiment: container-level cgroup memory pressure is meant to drive database buffer-pool adaptation. It is **educational/experimental**, explicitly not a Docker or PostgreSQL replacement (README Overview). The codebase is internally consistent, well-commented, and honest about its limits (documented durability boundary, namespace privilege requirements, ENV-LIMITED test pattern).

### What demonstrably works (verified by source + live execution)

| Area | Evidence |
|------|----------|
| Container lifecycle state machine (`CREATED → RUNNING → STOPPED → DELETED`) with invalid-transition rejection | `src/container/container.c:362-489`, live `nexusdb create/start/stop/delete/status` |
| Real cgroups v2 enforcement (`memory.max`/`cpu.max`) with read-back verification; attach verified via `cgroup.procs`; destroy refuses non-empty cgroups | `src/container/cgroup.c` |
| Linux namespace syscalls (`clone`/`unshare` with `CLONE_NEW*`); documented `CAP_SYS_ADMIN` requirement, no silent unisolated start | `src/container/namespace.c`, `include/container/namespace.h` |
| Gated fork/exec workload launch (exec-failure reported via CLOEXEC status pipe); `SIGTERM`→`SIGKILL`→`waitpid` stop with zombie reaping; `SIGHUP` ignored by workload | `src/container/process.c` |
| Page storage: fixed 4 KiB pages, write-through `WritePage`, `AllocatePage` | `include/db/config.h:20`, `src/db/storage/page_storage.cpp` |
| Buffer pool: true LRU, pinning, dirty writeback-before-reuse, abort-safe resize | `src/db/buffer/buffer_pool.cpp` |
| Slotted-page + heap-table record layer with tombstones, compaction, cycle-protected directory walks | `src/db/record/slotted_page.cpp`, `src/db/record/heap_table.cpp` |
| WAL: physiological page-image records, LSN-monotonic, CRC-32, loud corruption, truncated-tail tolerance, write-ahead ordering | `src/db/wal/wal.cpp`, `include/db/wal.h` |
| Database facade: atomic fresh-DB two-phase publish, recovery-before-pool open, checkpoint on close | `src/db/database/database.cpp` |
| `db_engine` executable: Unix-domain control socket, WSL2 `drvfs` socket relocation, graceful flush-on-exit, exit codes 0/2/3/4/5/6 | `src/db/engine/db_engine.cpp` |
| CLI + Router: documented exit-code contract 0/1/2/3/4 implemented; memory-suffix and CPU validation; hex validation; stdout-if-ok/stderr-if-error routing | `src/manager/router.cpp`, `src/manager/manager.cpp` |
| Shell integration: unified `nexus` wrapper, prompt base/suffix/restore, CLI path resolution, `set -u`-safe, idempotent sourcing | `scripts/bash-integration.sh` |
| Tests: `ctest --test-dir build-linux-clean` → **29/29 pass** (includes this audit's `cli_bash_prompt_integration` test); bash integration test → **81/81 pass** | live `ctest`, live test run |
| Compiler warnings: `-Wall -Wextra -Werror` in both build systems; incremental rebuild clean | `CMakeLists.txt:25`, `Makefile:28-29` |

### Major correctness risks (P1/P2)

1. **No power-loss/OS-crash durability:** WAL/database durability = C++ stream `flush()` (OS-level, survives process crash/`_exit`/`SIGKILL`) — explicitly **not** `fsync`, so it does not survive an OS crash or power loss. Documented in `include/db/wal.h:30-37`, but easy to misread as full durability. (P2)
2. **Namespace isolation is not on by default:** `container_create_ex()` is called with `namespaces == NULL` from `Manager::ContainerCreate` (`src/manager/manager.cpp:354-355`), so a `nexusdb create testdb --no-engine` workload runs in the caller's namespaces — the runtime *supports* isolation but the CLI never requests it. Isolation claims in docs must be read as opt-in at the API level, not default behavior. (P2 documentation/clarity)
3. **Mount namespace has no new root:** `namespace_configure_child` only makes mount propagation private; no `pivot_root`/`chroot`, so the workload sees the host filesystem view unless it mounts over it. Documented in `include/container/namespace.h:22-24`. (P2)
4. **PID reuse / weak process identity before signalling:** `process_is_alive` uses `kill(pid,0)` (counts zombies as alive) and `container_stop` sends `SIGTERM` to the recorded PID with no cmdline/start-time identity check. A recycled PID could be signaled by mistake (stop would still report success via `waitpid` → `ECHILD`). (P2/P3)
5. **Context staleness:** the Bash `nexus enter` context survives container deletion (the shell has no auto-validation). Documented as cosmetic selection, not isolation; acceptable for the stated design but worth noting. (P3)

### Major security risks

None rising to P0. The process runs as the invoking user (no `setuid`, no privilege escalation — `namespace.h:11-18` explicitly refuses `CAP_SYS_ADMIN` escalation). Attack surfaces are local: cgroup file writes, Unix-domain socket (mode `0600`, `db_engine.cpp:470`), and the control-socket protocol. Isolation is weaker than container-orchestration grade by design (no seccomp, no capabilities drop beyond namespaces, no seccomp, mount namespace without new root). See Section F.

### Major reliability risks

- Durability boundary (flush-not-fsync) — process-crash safe, power-loss unsafe.
- Default `sleep 3600`/`db_engine` workload runs unisolated unless namespace config is supplied (CLI doesn't expose it).
- `delete` on a running container is refused (`NEXUS_ERR_STATE`); delete of a stopped container whose cgroup still holds live (e.g. crashed-or-orphaned) processes fails with `NEXUS_ERR_STATE` — correct but can require manual cgroup drainage (observed during this audit: an orphaned `sleep 3600` from an interrupted test run blocked `delete` until killed). This is runtime-correct invariant behavior, not a bug.

### Most important next task

**Add `fsync` durability (or at least a documented, configurable durability mode) to the WAL/database write path** — this is the single highest-impact correctness gap because every other layer (recovery, engine shutdown, buffer-pool flush-all) already builds on the assumption that `BeforePageWrite` → flush is the durability boundary. Acceptance: a new `durability.md`-style test that `fsync`s the WAL/database before acknowledging commit, verified to survive `fsync`-level crash simulation (process kill after flush) and documented as NOT surviving power loss without it.

---

## B. Architecture Map

```
                    +---------------------+
                    |   Bash shell        |
                    |  `nexus` wrapper    |  (scripts/bash-integration.sh)
                    +---------------------+
                           | forwards argv
                           v
                    +---------------------+
                    |  nexusdb CLI        |  (src/cli/nexusdb_cli.cpp)
                    |  Router (parse+     |
                    |   validate argv)    |  (src/manager/router.cpp)
                    +---------------------+
                           |
            +--------------+--------------+
            |                             |
            v                             v
   +------------------+           +------------------+
   | Manager          |           | db_engine exec   |
   | (lifecycle + DB  |           | (control socket) |
   |  ops via runtime)|           |                  |
   +------------------+           +------------------+
            |                             |
   +--------+--------+           +---------+---------+
   |                 |           |                   |
   v                 v           v                   v
+---------+   +------------+  +----------+  +-------------+
| container|   | cgroup v2  |  | namespace|  | Database    |
| runtime  |   | (memory./  |  | (clone/  |  | (PageStorage|
| (64-slot |   | cpu.max/   |  | unshare) |  |  + WAL +    |
| registry)|   | procs,     |  | CLONE_*) |  |  BufferPool)|
+---------+   +------------+  +----------+  +-------------+
            |                           |
            +------> persisted meta <---+
                  (.nexus/containers/*.meta,
                   source of truth across
                   one-shot CLI invocations)
```

### Components and responsibilities

| Component | Files | Responsibility |
|-----------|-------|----------------|
| Bash integration / unified `nexus` | `scripts/bash-integration.sh` | Prompt suffix (`$:name>`), `enter/leave/exit/context`, CLI path resolution (`NEXUSDB_CLI` → `PATH` → sibling build dirs), forwarding with argv/exit-code/output fidelity |
| CLI executable | `src/cli/nexusdb_cli.cpp` | Thin front end: Router → print result → stable exit code |
| Router | `src/manager/router.cpp` | argv parsing/validation (memory suffixes, CPU 0..100, hex, page/slot ranges), dispatch, exit-code mapping, engine-socket protocol translation |
| Manager | `src/manager/manager.cpp` | Container lifecycle routed to runtime; persisted metadata (`<data_root>/.nexus/containers/<name>.meta`) + `container_adopt` rehydration; DB ops via `Database`; engine ping-verification on start; socket relocation for WSL2 `drvfs` |
| Container runtime | `src/container/{container,cgroup,namespace,process}.c` | 64-slot registry, lifecycle state machine, cgroups v2 (`memory.max`/`cpu.max`/`cgroup.procs`), Linux namespaces (`CLONE_NEW*` via `clone`/`unshare`), gated fork/exec workload launch, stop (`SIGTERM`→`SIGKILL`→reap) |
| db_engine | `src/db/engine/db_engine.cpp` | Long-running workload: opens `Database`, serves newline-delimited Unix socket (`ping/stats/table/*/insert/get/update/delete/scan/scan/quit`), graceful WAL-ordered flush on `SIGTERM`/`SIGINT`, socket relocation for `drvfs` (`EOPNOTSUPP`) |
| Database | `src/db/database/{database.cpp}` | Open/recovery facade: WAL recovery before pool, flush-all + checkpoint on close, atomic two-phase fresh-DB publish |
| Buffer pool | `src/db/buffer/buffer_pool.cpp` | True LRU, pinning, dirty writeback, abort-safe resize, RAII `Handle` unpin |
| Page storage | `src/db/storage/page_storage.cpp` | Fixed 4 KiB pages, dense ids, write-through `WritePage` |
| Record layer | `src/db/record/{slotted_page,heap_table}.cpp` | Slotted pages with tombstones + compaction; heap tables via directory-page chains; CRUD by `(page, slot)` record ids |
| WAL | `src/db/wal/wal.cpp` | Append-only CRC-32 page-image log, LSN-monotonic, replay last-image-wins, loud corruption, truncated-tail tolerance |
| Catalog / QueryExecutor | `src/db/catalog/catalog.cpp`, `src/db/query/query_executor.cpp` | Table-scoped CRUD + scan |
| Bridge / adaptation / dashboard | `src/bridge/*`, `src/dashboard/*` | cgroup metric sampling → grow/shrink policy → `BufferPool::Resize`; dashboard text/ncurses |
| Benchmarks | `benchmarks/`, `src/benchmark/*` | Real engine runs under cgroup memory limit; honesty contract (unavailable metrics listed) |

### Coupling notes
- The CLI is a **one-shot process per command**: the process-local container registry (64 slots) cannot see containers from prior invocations; rehydration relies entirely on persisted `.meta` files + `container_adopt`. This is a deliberate, documented trade-off.
- The `Manager` owns both runtime and DB concerns (no clean boundary violation, but a large coordinator class).
- WAL durability ordering is enforced at the single choke point `BufferPool::FlushFrame` → `Wal::BeforePageWrite` → flush before `PageStorage::WritePage`.

---

## C. Feature Verification Matrix

| Feature | Implemented | Verified | Evidence | Limitations |
|---|---|---|---|---|
| Container lifecycle (create/start/stop/delete/status/list) | Yes | Yes | `src/container/container.c:362-489`; live `nexusdb` run | delete refuses RUNNING; cgroup must be empty |
| Namespace isolation (PID/mount/net/UTS/IPC) | Partial (API yes; CLI default no) | Partial (API) | `container_create_ex()` called with `namespaces==NULL` from `manager.cpp:354`; `namespace.c` real syscalls | isolation opt-in via API only; mount ns has no new root |
| cgroups v2 enforcement (`memory.max`/`cpu.max`) | Yes | Yes | `cgroup.c` set_limits read-back + attach verify + destroy refuses non-empty | requires delegated cgroup subtree (ENV-LIMITED otherwise) |
| Page storage (4 KiB, dense ids) | Yes | Yes | `config.h:20`, `page_storage.cpp` | fixed page size only |
| Buffer pool (LRU, pin, dirty, resize) | Yes | Yes | `buffer_pool.cpp` | shrink refuses if pinned frames insufficient |
| SlottedPage records (insert/read/update/delete/scan/compact) | Yes | Yes | `slotted_page.cpp` | tombstones = fragmentation until Compact() |
| HeapTable (directory chains, multi-page) | Yes | Yes | `heap_table.cpp` | record ids are `(page, slot)`; no secondary index |
| Catalog / table metadata | Yes | Yes | `catalog.cpp`, `query_executor.cpp` | name→table only; no schema/types |
| WAL (CRC-32 page images, recovery) | Yes | Yes | `wal.cpp`, `database.cpp` recovery-before-pool | flush≠fsync (no power-loss durability) |
| Crash recovery | Yes (process-crash) | Partial | `CrashPoint` injection in `database.cpp` + `recovery_test` | power-loss/unverified |
| Memory-pressure bridge → buffer-pool resize | Yes | Yes | `bridge/adaptation_policy.c`, `engine_adaptation` ctest | CPU monitoring-only, never drives resize |
| `db_engine` control socket + WSL2 relocation | Yes | Yes | `db_engine.cpp` | `drvfs` EOPNOTSUPP → relocation |
| CLI exit-code contract (0/1/2/3/4) | Yes | Yes | `router.cpp MapStatus`; live runs | operational errors all collapse to 1 |
| Shell integration prompt + context | Yes | Yes | `bash-integration.sh`; 81-check bash test | cosmetic suffix; no isolation; stale after delete |
| Build (`-Wall -Wextra -Werror`) | Yes | Yes | `CMakeLists.txt:25`; incremental + full build clean | |
| Test suite (ctest) | Yes | Yes | 29/29 pass | several tests ENV-LIMITED without delegated cgroup |

---

## D. Build and Test Results

### Commands executed
1. `cmake --build build-linux-clean --parallel` → 100% targets built, **no warnings** (`-Wall -Wextra -Werror` in `CMakeLists.txt:25`, `Makefile:28-29`).
2. `ctest --test-dir build-linux-clean` → **29/29 passed**, 0 failed, ~22 s.
3. `bash tests/cli/bash_integration_test.sh build-linux-clean/nexusdb` → **81/81 passed** (prompt + unified-command + delegation + stream-fidelity + unload checks).
4. Live DoD flow via `bash -i` (isolated temp `NEXUS_DATA_ROOT`): `create → start → status → enter testdb → db init → table create → insert ×2 → scan → context → leave → stop` all correct; `enter ayush` (nonexistent) rejected rc=4, prompt/context unchanged; restart restore verified byte-exact (`RESTORED_EXACTLY`) against the real colored Ubuntu PS1.

### Test inventory (from build tree)
`container_lifecycle`, `container_engine_demo`, `cgroup_reliability`, `process_exec`, `namespace_isolation`, `db_engine_binary`, `bridge_resource_monitor`, `memory_bridge`, `bridge_adaptation_policy`, `dashboard_metrics`, `dashboard_demo`, `benchmark_harness`, `db_storage`, `slotted_page`, `heap_table`, `catalog`, `query_executor`, `wal`, `recovery`, `buffer_pool`, `buffer_pool_demo`, `nexusdb_engine_demo`, `cpu_enforcement`, `memory_enforcement`, `manager`, `cli`, `engine_adaptation`, `dashboard_engine`, `cli_bash_prompt_integration`.

### Environment limitations honestly reported by the project
Tests requiring a delegated cgroup v2 subtree (`namespace`, `cpu_enforcement`, `memory_enforcement`, plus benchmarks) print **ENV-LIMITED** and exit 0 instead of claiming results, when run outside a delegated systemd user scope. WSL2 interactive shells start outside systemd's user cgroup; the CLI itself auto-delegates `start` via `systemd-run --user --scope` (`cli/nexusdb_cli.cpp:70-117`), which worked in this environment.

### Build warnings / strictness
`-Wall -Wextra -Werror` applied in both build systems. Incremental rebuild produced no warnings. Claim in `docs/container-runtime.md:66` ("All tests pass cleanly with -Wall -Wextra -Werror") is consistent with observation. Two older branch logs (`build-linux-p0p4.build.log`, `build-make-p0p4.build.log`) contain "warning" mentions but belong to a different build configuration, not the audited tree.

---

## E. Findings Register

| ID | Severity | Component | File:line | Evidence | Impact | Recommended remediation | Test to verify |
|---|---|---|---|---|---|---|---|
| AUD-01 | P2 | Durability / WAL | `include/db/wal.h:30-37`, `src/db/wal/wal.cpp:298-308` | `Wal::Flush()` calls `file_.flush()` only; doc states "does NOT by itself survive an OS crash or power loss ... additionally require fsync" | Process-crash safe only; OS crash / power loss → possible acknowledged-but-lost writes | Add `fsync()`/`fdatasync()` (or `fflush`+`fsync` on the underlying fd) at the WAL flush + database write-through points; document durability mode | New test: write + `fsync`, `kill -9` the process, reopen, verify record survives |
| AUD-02 | P2 | Isolation defaults | `src/manager/manager.cpp:354-355`, `src/container/container.c:118` | `ContainerCreate` → `container_create_ex(..., NULL)` → no namespaces requested; default workload (`sleep 3600`/`db_engine`) runs in caller's namespaces | Users may assume isolation by default; docs say lifecycle "places the workload in the requested namespaces" but CLI never requests any | Either wire namespace config through `create` (with a CLI flag) or document emphatically that isolation is opt-in via `container_create_ex` API only | Integration test asserting `ns_*` fields are 0 after default `create`, and non-zero when a namespace config is supplied |
| AUD-03 | P2 | Mount namespace | `include/container/namespace.h:22-24`, `src/container/namespace.c:85-89` | Only `mount("none","/",NULL,MS_REC|MS_PRIVATE)`; no `pivot_root`/`chroot`; workload sees host FS view | Mount isolation is partial by design | Document clearly; consider optional rootfs provisioning if stronger isolation is ever a goal | None (documented limitation) |
| AUD-04 | P2/P3 | Process identity | `src/container/process.c:269-277` (`kill(pid,0)`), `container.c:453-456` (`container_stop` signals recorded pid) | `is_alive` counts zombies as alive; `stop` signals recorded PID with no cmdline/start-time verification; `waitpid` on non-child → `ECHILD` → success | PID reuse could cause `stop` to signal an unrelated live process, yet still report success | Before signalling, verify the recorded pid's cmdline/executable matches the expected workload (e.g. `/proc/<pid>/cmdline` on Linux) | Unit test: spawn placeholder, note pid, run stop; assert only the expected process was signaled |
| AUD-05 | P3 | Shell context staleness | `scripts/bash-integration.sh` (`NEXUSDB_CONTEXT` never auto-validated) | `nexus enter testdb` → container deleted elsewhere → context still reports `testdb` and prompt keeps suffix | Misleading prompt after container deletion (cosmetic; no data risk) | Optional `nexus context --verify` that re-runs `nexusdb status` and clears a stale context | Test: enter, delete container via CLI directly, run context, assert cleared |
| AUD-06 | P3 | Error-code granularity | `src/manager/router.cpp:142-158` (`MapStatus` default → `kError`=1) | All operational errors (`kIoError`, `kBadState`, `kBufferFull`, `kOutOfMemory`) collapse to exit code 1 | Callers cannot distinguish "container busy" from "out of memory" by exit code alone | Consider finer exit codes or structured error output for operational failures | Unit test mapping each `DbError` to expected exit code |
| AUD-07 | Info | Stale comment | `include/container/container.h:28-29` | "This dev machine has no WSL" while audited environment is WSL2 | Minor confusion | Update comment | N/A |
| AUD-08 | Info | Stale test count | `README.md` Project Status ("26 tests are registered with CTest") | Current tree registers 29 ctest tests | Minor doc drift | Update count or make it example-only | N/A |

Note: several findings (AUD-02, AUD-03) are **documented limitations**, not defects — the project is honest about them in `namespace.h` and `container-runtime.md`. They are registered because the *default CLI path* does not exercise the isolation the API supports.

### Remediation status addendum — 2026-11-24

- **AUD-01 (durability): resolved.** `include/common/durable.h` provides a real `fsync()` boundary; `Wal::Flush()` calls `nexusdb::FsyncFile()` after the stream flush, and `PageStorage` fsyncs files at close/write-through points. The historical "flush only" statements above describe the audit snapshot, not the current tree.
- **AUD-02 (isolation defaults): addressed with explicit opt-in.** `nexusdb create <name> --isolate` / `--namespace` persists `ns_pid`, `ns_mount`, `ns_net`, `ns_uts`, and `ns_ipc` metadata and rehydrates it through `container_adopt_ex()`. Default create remains unisolated by design; docs and CLI help now state this.
- **AUD-03 (mount namespace): documented limitation remains.** The mount namespace makes mounts private but does not `pivot_root`/`chroot`; docs state that host filesystem visibility is intentional and not Docker-equivalent isolation.
- **AUD-04 (process identity/stop): resolved.** `process_identity()` records the kernel PID start ticks and process name; `process_identity_matches()` is checked before `container_stop()` / `container_delete()` signal a recorded PID. The identity is persisted in container metadata and rehydrated. Stale identities are refused with `NEXUS_ERR_STATE`; namespaced PID 1 workloads are still force-stopped with `SIGKILL`.
- **AUD-05 (shell context): resolved.** `nexus context --verify` validates the active container through the CLI, clears a stale `NEXUSDB_CONTEXT`, and restores the base prompt. It avoids a CLI check on every prompt render.
- **AUD-06 (error codes): resolved.** `CliExit` now defines stable codes `5` I/O error, `6` bad state, `7` buffer full, and `8` out of memory, mapped from `DbError` through `cli::ExitCodeFor()`.
- **Cache/latency honesty:** `BufferPool` exposes real hit/miss counters; `db_engine stats` reports `cache_hits`, `cache_misses`, and last successful query latency (`n/a` before the first query). The benchmark harness still reports only harness-observed `Contains()`/`Fetch` timings and says so explicitly.
- **Live resource reporting:** `nexusdb resources <name>` / `nexusdb container resources <name>` exposes a live cgroup snapshot distinct from configured limits.
- **Verification:** Final WSL CTest after all Phase 2 changes is 30/30 with only genuine ENV-LIMITED namespace skips for non-root users; Windows CTest is 29/29 with platform skips for Unix-only tests. Root-only namespace/lifecycle targeted runs pass with real namespace creation. The end-to-end demo transcript was regenerated against the final code and ends with `=== END-TO-END DEMO PASSED ===`.
- **Still open by design:** AUD-03's new-root/host-FS visibility limitation and the project's explicit non-ACID/non-Docker scope. No unresolved remediation from AUD-01, AUD-02, AUD-04, AUD-05, or AUD-06 remains.

---

## F. Security and Threat Model

### Trust boundaries
- **Trust boundary:** the invoking local user. NexusDB runs with the invoking user's privileges; there is no `setuid`, no capability escalation (`namespace.h:11-18` explicitly: "never elevates privileges"; unprivileged namespace creation → hard `EPERM` failure, never silent unisolated start).
- **Trusted:** the user who creates containers, the local cgroup delegation (systemd user scope), the invoking shell.
- **Untrusted:** container workloads (placeholder `sleep 3600` or user-supplied `--engine` binary path).

### Implemented protections
- Container names validated to `[A-Za-z0-9_-]`, length < 64 (`container.c:36-50`, `manager.cpp:226-235`) — no path traversal/shell injection via name.
- cgroup names reject `/`, `\`, `.`, `..` (`cgroup.c:374-390`).
- Control socket created mode `0600` regardless of umask (`db_engine.cpp:468-470`).
- Socket marker file records actual path; engine fails fast (exit 6) if no socket can be created — never serves without a socket.
- Engine workload ignores `SIGHUP` (survives terminal close) but dies on explicit `SIGTERM`/`SIGINT`; no network listener.
- WAL checksums (CRC-32) and loud corruption failures — no silent acceptance of corrupt logs.
- No SQL → no SQL injection surface.

### Absent / out-of-scope protections
- No seccomp, no capabilities drop beyond namespaces, no AppArmor/seccomp profile.
- Mount namespace without new root → host FS visible (documented).
- NET namespace = loopback-only, DOWN, no veth/NAT → no external connectivity by design (not a bypass; no connectivity at all).
- PID reuse weakness (AUD-04).
- No authentication/authorization on the control socket (local filesystem permissions only).
- Does not provide Docker-equivalent isolation — README states this explicitly.

### Attack surface summary
Local: cgroup file writes (require writable delegated cgroup subtree), control socket (`0600`, same-user), CLI argument parsing (validated), WAL/database files (user-writable data dir). Remote: none (no network).

---

## G. Data Integrity and Recovery Assessment

### What the implementation actually guarantees

1. **Process-crash safety (not power-loss):** WAL page-image records are flushed (`ostream::flush`) before the corresponding page is written to `PageStorage` — enforced at the single choke point `BufferPool::FlushFrame` → `Wal::BeforePageWrite` (`buffer_pool.cpp:261-282`, `wal.cpp:310-319`). Recovery replays valid images in LSN order, last-image-wins, idempotent (`wal.cpp:359-412`). `_exit`/`SIGKILL`/process crash → next `Database::Open` replays the WAL. ✓ Verified by `recovery_test` and `CrashPoint` injection points (`database.cpp:66-75`).
2. **Atomic fresh-database creation:** build at `.creating` temp path, publish via `rename`; stale WAL cleared before publish; crash injection between every stage (`database.cpp:121-212`). ✓ Verified by `recovery_test`.
3. **Graceful shutdown:** `db_engine` on `SIGTERM`/`SIGINT` → `Database::Close()` → `pool_->FlushAll()` (write-ahead ordered via log sink) → `wal_->Reset()` checkpoint → `storage_->Close()` → exit 0. ✓ Verified live (`db_engine: clean shutdown pid=...`).
4. **Corruption handling:** WAL fully-present-but-invalid record → `kBadState`, never silently skipped (`wal.cpp:115-187`, `ReadRecord`); database header magic/version/catalog-root validated on open (`database.cpp:81-105`); truncated WAL tail tolerated (`RecoveryStats.truncated_tail`).

### What is NOT guaranteed / where data loss remains possible
- **Power loss / OS crash:** durability boundary is `flush()` only, no `fsync` (documented in `wal.h:30-37`). Acknowledged writes can be lost on OS crash/power failure.
- **Buffer-pool pages never flushed:** a dirty page that is never evicted/flushed is not recovered (documented `wal.h:19-22` — "expected, as it was never made durable").
- **Database file truncation/malformation:** opening a foreign/corrupt file fails loudly (`kBadState`), but a truncated-but-header-valid file with missing pages would fail at page read → `kIoError`/`kNotFound`; not auto-repaired.
- **Record-level granularity:** WAL logs full page images, so record updates/deletes are captured at page granularity — correct, but means a single-record change rewrites the whole page image.
- **Concurrent file access:** no file locking between two `Database` opens on the same file; `OpenDb` refuses direct access while RUNNING (manager.cpp:586-590), but two stopped-container CLI invocations racing each other on the same files are not serialized by the engine.

### ACID statement
NexusDB does **not** claim ACID compliance. It provides single-process, page-image WAL recovery against process crashes, with an explicit flush-not-fsync durability boundary. Transactions, multi-statement atomicity, and distributed durability are in "Planned" (README), not implemented.

---

## H. Prioritized Development Roadmap

Per the task's prioritization framework (data integrity → lifecycle → isolation → cgroups → storage → WAL → buffer pool → CLI → tests → performance → features → docs), the roadmap:

### Phase 1 — Durability correctness (Small–Medium)
**Problem:** WAL/database durability is flush-not-fsync; no power-loss/OS-crash guarantee.
**Files:** `src/db/wal/wal.cpp`, `src/db/storage/page_storage.cpp`, `include/db/wal.h`
**Approach:** add `fsync()`/`fdatasync()` at WAL flush and `PageStorage::WritePage` write-through; expose a durability mode (process-safe vs power-safe) documented in `wal.h`.
**Acceptance:** new test kills process after flush+fsync and verifies record survives reopen; README/durability doc updated; `-Wall -Wextra -Werror` clean; ctest green.
**Risk:** small (performance cost of fsync is acceptable for correctness).

### Phase 2 — Lifecycle & identity hardening (Small)
**Problem:** PID reuse / weak identity before signalling (AUD-04); delete blocked by orphaned cgroup processes requires manual drainage (correct invariant, but operationally painful).
**Files:** `src/container/process.c`, `src/container/container.c`
**Approach:** before `container_stop`/`delete` signals the recorded pid, verify `/proc/<pid>/cmdline` (or equivalent) matches the expected workload; on stop, wait/reap until the pid is truly gone before reporting success.
**Acceptance:** unit test asserts stop signals only the expected process; delete succeeds after process exit without manual intervention.

### Phase 3 — Isolation clarity (Small, doc/CLI)
**Problem:** isolation supported by API but not requested by default `create` (AUD-02, AUD-03).
**Files:** `src/manager/manager.cpp`, `src/manager/router.cpp`, README
**Approach:** either add a `--isolate`/`--namespace` flag to `nexusdb create` (mapped to `container_create_ex` namespace config), or document emphatically that isolation is API opt-in; update `container-runtime.md` to state default create runs unisolated.
**Acceptance:** test asserts default-created container has no namespace fields set; documented behavior matches CLI.

### Phase 4 — Error-code granularity (Small)
**Problem:** all operational errors map to exit code 1 (AUD-06).
**Files:** `src/manager/router.cpp:142-158`
**Approach:** map `kIoError`/`kBadState`/`kBufferFull`/`kOutOfMemory` to distinct operational exit codes (keeping 0/2/3/4 contract intact).
**Acceptance:** unit test maps each `DbError` to exit code.

### Phase 5 — Shell context validation (Small–Medium)
**Problem:** `nexus enter` context survives container deletion (AUD-05).
**Files:** `scripts/bash-integration.sh`
**Approach:** optional `nexus context --verify` (or lazy verify on `enter`/`context`) that re-runs `nexusdb status` and clears a stale context.
**Acceptance:** bash test: enter → delete via CLI → `context` reports none / prompt restored.

### Phase 6 — Test coverage expansion (Medium)
**Problem:** namespace/cgroup enforcement tests are ENV-LIMITED without a delegated cgroup subtree; several error paths lack direct unit coverage (AUD-04, AUD-06).
**Files:** `tests/container/*`
**Approach:** add unit tests that run under `unshare -U --map-root-user` where possible; keep ENV-LIMITED fallback honest.
**Acceptance:** new tests pass in delegated scope; ENV-LIMITED path still exits 0 without it.

### Phase 7 — Performance / benchmarks (Medium, optional)
**Problem:** benchmarks measure buffer-pool resizing under pressure (real, honest) but not record-level SQL-like throughput (no SQL exists — out of scope).
**Approach:** maintain existing benchmark honesty contract; no new synthetic comparisons to external DBs.
**Acceptance:** benchmark README stays consistent with code; `run_benchmarks.sh` completes under delegated cgroup.

### Phase 8 — Additional DB features (Large — deferred)
SQL parser/execution, transactions, networking, replication are in "Planned" and explicitly not on this branch. Defer until Phase 1–6 land.

---

## I. Recommended Next Task

**Task:** Add `fsync`-based durability to the WAL/database write path (Phase 1, AUD-01 remediation).

**Why first:** Every layer above (recovery, engine shutdown flush-all, buffer-pool dirty writeback, `BeforePageWrite` write-ahead rule) builds on the assumption that a flushed WAL record is durable. Currently that assumption holds only against process crashes; against OS crash/power loss it is false and undetectable at the application layer. Fixing the boundary here strengthens every dependent feature (CRUD, recovery, engine adaptation tests) at once, and it is a contained change with a clear acceptance criterion.

**Implementation plan:**
1. In `Wal::Flush()` (`wal.cpp:298-308`), after `file_.flush()`, obtain the underlying fd (`file_.fd()` is not standard; use a platform fd accessor or reopen via `fileno` on the streambuf's file descriptor) and call `fsync()` (POSIX) / `_commit` (Windows).
2. In `PageStorage::WritePage` write-through path, `fsync()` the database file after write (or provide a `DurableWritePage` variant used by `FlushFrame`).
3. Update `include/db/wal.h` durability-boundary doc to state the new guarantee and its limits.
4. Add `tests/db/wal_crash_test.cpp` (or extend `recovery_test`): write a record, flush+fsync, `kill -9` the writer, reopen via `Database::Open`, verify the record is present; verify pre-fsync data may be lost (documenting the boundary).
5. Run `ctest` (29/29 target) and the bash integration test; ensure `-Wall -Wextra -Werror` clean.

**Acceptance criteria (measurable):**
- `ctest --test-dir build-linux-clean` → 29/29 (or N+1 with the new test) passed.
- `bash tests/cli/bash_integration_test.sh build-linux-clean/nexusdb` → 81/81.
- New durability test passes: post-`fsync` kill-9 → record survives reopen.
- Docs state durability boundary precisely (process-crash yes, power-loss no unless fsync path used).

**Risk:** low. `fsync` on Linux/WSL2 is standard; performance impact is acceptable for an educational engine; fallback to `flush`-only retained if `fsync` unavailable.

---

## J. Known Limitations

- **Durability:** flush-to-OS only; no `fsync` → no power-loss/OS-crash durability (documented, `wal.h`).
- **Isolation:** namespaces supported by API but not requested by default `create`; mount namespace lacks new root (host FS visible); NET loopback-only; requires `CAP_SYS_ADMIN` for real namespaces, no user-namespace creation by the runtime.
- **Single-user, local-only:** no authentication on control socket; no network; runs as invoking user.
- **One-shot CLI:** process-local registry rehydrated from persisted `.meta`; no cross-process live registry.
- **Environment-dependent tests:** cgroup/namespace enforcement tests report ENV-LIMITED (exit 0) without a delegated cgroup v2 subtree; by design, never claim unearned results.
- **Buffer-pool shrink:** refuses (kBufferFull) when pinned frames prevent reaching target capacity.
- **No SQL, transactions, networking, replication** — all in "Planned", explicitly not implemented.
- **Windows dev shim:** compiles/runs on MinGW with mock cgroup telemetry and placeholder workload; real enforcement is Linux-only.

---

## Summary (final response)

**Overall project health:** Good for its stated scope. NexusDB is a small, internally consistent, honestly-documented experimental OS↔database integration. The code that exists is well-engineered (validated inputs, write-ahead ordering, abort-safe resize, loud corruption, ENV-LIMITED honesty for environment-dependent tests), builds clean under `-Wall -Wextra -Werror`, and passes its full test suite (29/29 ctest; 81/81 bash integration checks).

**Most important verified successes:**
- Container lifecycle state machine with invalid-transition rejection; real cgroups v2 `memory.max`/`cpu.max` enforcement with attach verification.
- Gated fork/exec workload launch; `SIGTERM`→`SIGKILL`→reap stop with zombie prevention.
- Page-image WAL with CRC-32, monotonic LSN, loud corruption, truncated-tail tolerance, recovery-before-pool open.
- Documented exit-code contract 0/1/2/3/4 implemented and live-verified.
- Shell integration: prompt suffix `:<name>>`, switch/replace/leave restore, CLI path resolution, `set -u`-safe, idempotent sourcing — all verified.

**Top five findings:**
1. **AUD-01 (P2)** No `fsync` → durability is process-crash-safe only, not power-loss-safe (documented boundary).
2. **AUD-02 (P2)** Namespace isolation exists in the API but default `create` requests none — runs unisolated.
3. **AUD-03 (P2)** Mount namespace has no new root → workload sees host FS (documented).
4. **AUD-04 (P2/P3)** `stop` signals the recorded PID with no identity verification beyond `kill(pid,0)` — PID-reuse hazard.
5. **AUD-05 (P3)** Bash `nexus enter` context survives container deletion (stale-but-cosmetic).

**Build and test results:** `cmake --build build-linux-clean --parallel` → 100%, no warnings; `ctest` → 29/29 pass; bash integration → 81/81 pass; DoD flow verified interactively. (Pre-existing tree dirtiness: 38 modified + 6 untracked files, not introduced by this audit.)

**Unverified areas:** power-loss durability (by design, not claimed), namespace enforcement under a delegated cgroup (ENV-LIMITED in this WSL2 interactive shell), record-level resize+recovery beyond the existing `engine_adaptation` test (which passed).

**Recommended next task:** Add `fsync` durability to the WAL/database write path (Section I), with acceptance: new crash test passes, `ctest` and bash integration stay green, docs updated.

**Complete audit report:** `docs/audit/NEXUSDB_AUDIT_REPORT.md` (this file). No runtime features or user data were changed during the audit.
