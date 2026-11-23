/*
 * NexusDB — Engine-backed live dashboard demo (Rehan).
 *
 * Unlike dashboard_demo.c (which has no engine and honestly renders the
 * database groups as "unavailable"), this demo attaches a REAL BufferPool over
 * PageStorage and shows the complete pipeline end to end:
 *
 *   container cgroup (real kernel counters)
 *     -> OS-DB Bridge snapshot (nexus_bridge_read_container)
 *       -> adaptation policy (nexus_policy_apply)
 *         -> production resize adapter (nexus_bridge_buffer_pool_request)
 *           -> real BufferPool capacity/resident/pinned
 *             -> dashboard metrics + plain-text render
 *
 * Every number shown is read from the kernel or the engine's public API;
 * cache-hit counters and query latency genuinely do not exist on the engine and
 * stay "unavailable" rather than being faked. The resource numbers come from
 * the container's own workload (the built-in placeholder child); the buffer
 * pool lives in this process, so its capacity/resident/pinned are real and the
 * bridge-driven resize is applied to it for real.
 *
 * Usage: nexus_dashboard_engine [name] [memory_mb] [iterations]
 *   iterations 0 = run until Ctrl-C.
 *
 * Linux/WSL2 only: it needs cgroups v2 and the real C++ engine. On Windows the
 * engine's <filesystem> use does not compile, so this target is not built there.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "bridge/adaptation_policy.h"
#include "bridge/buffer_pool_adapter.h"
#include "bridge/resource_monitor.h"
#include "common/error.h"
#include "container/container.h"
#include "dashboard/dashboard.h"
#include "dashboard/metrics.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace {

constexpr unsigned long kNominalFrames = 256;
constexpr unsigned long kWindow = 32;
constexpr unsigned long kPageCount = 128;
constexpr int kSampleIntervalMs = 500;

// Context handed to the dashboard engine-metrics provider: the live pool plus
// the last policy target (so "requested" and "actual" can be compared).
struct EngineCtx {
  nexusdb::BufferPool* pool = nullptr;
  unsigned long target = 0;
};

int EngineMetricsProvider(void* ctx, nexus_engine_metrics_t* out) {
  auto* e = static_cast<EngineCtx*>(ctx);
  if (e == nullptr || e->pool == nullptr || out == nullptr) {
    return NEXUS_ERR_INVALID;
  }
  std::memset(out, 0, sizeof(*out));
  out->has_pool = 1;
  out->pool_target_frames = e->target;
  out->pool_actual_frames = static_cast<unsigned long>(e->pool->capacity());
  out->pool_resident_frames = static_cast<unsigned long>(e->pool->size());
  out->pool_pinned_frames = static_cast<unsigned long>(e->pool->pinned_frames());
  // has_cache_stats / has_latency stay 0: the engine exposes neither.
  return NEXUS_OK;
}

}  // namespace

int main(int argc, char** argv) {
  const char* name = (argc > 1) ? argv[1] : "nexusdb-engine";
  unsigned long memory_mb = (argc > 2) ? std::strtoul(argv[2], nullptr, 10) : 512UL;
  long iterations = (argc > 3) ? std::strtol(argv[3], nullptr, 10) : 6;

  if (memory_mb == 0) {
    memory_mb = 512UL;
  }

  std::printf("=== NexusDB Live Dashboard (engine-backed demo) ===\n");
  std::printf("cgroup root: %s\n", cgroup_root());
  std::printf("container:   %s (%lu MiB)\n", name, memory_mb);

  // --- Real engine: a BufferPool over a PageStorage in a temp file. ---
  const std::string dir =
      (std::filesystem::temp_directory_path() / "nexusdb_dashboard_engine")
          .string();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  std::unique_ptr<nexusdb::PageStorage> storage;
  std::unique_ptr<nexusdb::BufferPool> pool;
  {
    if (!nexusdb::PageStorage::Open(dir + "/engine.db", &storage).ok() ||
        storage == nullptr) {
      std::fprintf(stderr, "PageStorage::Open failed\n");
      return 2;
    }
    for (unsigned long i = 0; i < kPageCount; ++i) {
      nexusdb::Page tmp;
      if (!storage->AllocatePage(&tmp).ok()) {
        break;
      }
    }
    if (!nexusdb::BufferPool::Create(storage.get(), kNominalFrames, &pool).ok() ||
        pool == nullptr) {
      std::fprintf(stderr, "BufferPool::Create failed\n");
      return 2;
    }
  }

  // --- Container to measure: real cgroup with a memory (and CPU share) limit. ---
  int rc = container_create(name, memory_mb * 1024UL * 1024UL, 50, nullptr);
  if (rc == NEXUS_ERR_EXISTS) {
    (void)container_stop(name);
    (void)container_delete(name);
    rc = container_create(name, memory_mb * 1024UL * 1024UL, 50, nullptr);
  }
  if (rc != NEXUS_OK) {
    /* No writable cgroup here (unprivileged shell / Windows-style shim): fall
     * back to a real engine-only frame and report ENV-LIMITED honestly rather
     * than claiming resource metrics we could not read. */
    std::fprintf(stderr,
                 "ENV-LIMITED: container_create('%s') failed: %s\n"
                 "  no writable cgroup is available; showing an engine-only frame.\n"
                 "  For the full pipeline run under a delegated cgroup scope, e.g.\n"
                 "    systemd-run --user --scope env NEXUS_CGROUP_ROOT=<writable-cgroup> ...\n",
                 name, container_strerror(rc));
    EngineCtx ectx;
    ectx.pool = pool.get();
    ectx.target = static_cast<unsigned long>(pool->capacity());
    nexus_dashboard_metrics_t m;
    if (nexus_dashboard_collect(nullptr, EngineMetricsProvider, &ectx, &m) ==
        NEXUS_OK) {
      char text[4096];
      if (nexus_dashboard_render_text(&m, text, sizeof(text)) == NEXUS_OK) {
        std::printf("\n%s", text);
      }
    }
    if (pool != nullptr) {
      (void)pool->FlushAll();
      pool.reset();
    }
    if (storage != nullptr) {
      (void)storage->Close();
      storage.reset();
    }
    std::filesystem::remove_all(dir, ec);
    std::printf("\n[ENV-LIMITED]\n");
    return 0;
  }

  (void)container_get(name); /* path fixed by the runtime; not needed here */

  rc = container_start(name);
  if (rc != NEXUS_OK) {
    std::fprintf(stderr,
                 "container_start('%s') failed: %s\n"
                 "  the cgroup is configured but limits are not enforced on a workload;\n"
                 "  the dashboard will honestly report \"Attached: no\".\n",
                 name, container_strerror(rc));
  }

  // --- Drive the loop: sample -> adapt -> resize the real pool -> render. ---
  nexus_policy_config_t cfg;
  nexus_policy_config_default(&cfg);
  nexus_policy_t policy;
  nexus_policy_init(&policy);

  nexus_resource_snapshot_t prev;
  std::memset(&prev, 0, sizeof(prev));
  bool have_prev = false;
  auto prev_t = std::chrono::steady_clock::now();
  unsigned long cursor = 0;

  for (long i = 0; (iterations <= 0) || (i < iterations); ++i) {
    // Visit a sliding window of pages so the pool has real resident frames and
    // the process does real work the kernel can charge CPU for.
    for (unsigned long k = 0; k < kWindow; ++k) {
      const nexusdb::Page::PageId id =
          static_cast<nexusdb::Page::PageId>((cursor + k) % kPageCount);
      nexusdb::BufferPool::Handle h;
      (void)pool->Fetch(id, &h);
    }
    cursor += kWindow;

    nexus_resource_snapshot_t snap;
    std::memset(&snap, 0, sizeof(snap));
    (void)nexus_bridge_read_container(name, &snap);

    const auto now = std::chrono::steady_clock::now();
    const long long elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - prev_t)
            .count();

    bool have_cpu = false;
    double cpu_pct = 0.0;
    if (have_prev && prev.has_cpu && snap.has_cpu) {
      const double pct = nexus_bridge_cpu_percent(&prev, &snap, elapsed_ms);
      if (pct >= 0.0) {
        have_cpu = true;
        cpu_pct = pct;
      }
    }
    prev = snap;
    have_prev = true;
    prev_t = now;

    // Bridge -> policy -> production adapter -> real pool.
    nexus_adapt_decision_t dec;
    std::memset(&dec, 0, sizeof(dec));
    (void)nexus_policy_apply(&policy, &cfg, &snap,
                             static_cast<unsigned long>(pool->capacity()),
                             nexus_bridge_buffer_pool_request, pool.get(), &dec);

    EngineCtx ectx;
    ectx.pool = pool.get();
    ectx.target = dec.target_frames;

    nexus_dashboard_inputs_t in;
    std::memset(&in, 0, sizeof(in));
    in.resource = &snap;
    in.has_cpu_percent = have_cpu ? 1 : 0;
    in.cpu_percent = cpu_pct;
    in.adaptation = &dec;

    nexus_dashboard_metrics_t m;
    if (nexus_dashboard_collect(&in, EngineMetricsProvider, &ectx, &m) !=
        NEXUS_OK) {
      std::fprintf(stderr, "frame collection failed\n");
      break;
    }

    char text[4096];
    if (nexus_dashboard_render_text(&m, text, sizeof(text)) == NEXUS_OK) {
      std::printf("\n%s", text);
    }

    if (iterations <= 0 || i + 1 < iterations) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kSampleIntervalMs));
    }
  }

  // --- Teardown: stop the workload, then delete the container. ---
  {
    const int stop_rc = container_stop(name);
    const int del_rc = container_delete(name);
    if (stop_rc != NEXUS_OK && stop_rc != NEXUS_ERR_STATE) {
      std::fprintf(stderr, "container_stop('%s') failed: %s\n", name,
                   container_strerror(stop_rc));
    }
    if (del_rc != NEXUS_OK) {
      std::fprintf(stderr, "container_delete('%s') failed: %s\n", name,
                   container_strerror(del_rc));
    }
  }
  if (pool != nullptr) {
    (void)pool->FlushAll();
    pool.reset();
  }
  if (storage != nullptr) {
    (void)storage->Close();
    storage.reset();
  }
  std::filesystem::remove_all(dir, ec);

  std::printf("\n[demo complete]\n");
  return 0;
}
