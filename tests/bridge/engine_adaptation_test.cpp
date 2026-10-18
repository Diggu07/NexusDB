// NexusDB — R4: OS-DB Bridge <-> Database engine integration test (Digvijay).
//
// This is the missing link the Phase-A audit identified: nothing previously
// drove the bridge's adaptation policy through the production resize adapter
// into a REAL BufferPool, and nothing proved that database records survive a
// bridge-requested resize and a close/reopen (WAL recovery).
//
// It exercises, against real engine objects (no mocks):
//   1. Policy decision math at synthetic memory-pressure levels (no engine, no
//      root): GROW / SHRINK / HOLD, clamping, hysteresis, duplicate suppression,
//      unlimited memory, missing metrics, invalid config.
//   2. The production adapter maps BufferPool::Resize onto NEXUS_* codes,
//      including the invalid-argument paths.
//   3. nexus_policy_apply() driving a real BufferPool: the requested
//      (target_frames) and actual (capacity()) capacities agree on success.
//   4. A shrink that cannot run because too many frames are pinned is refused
//      failure-atomically: the adapter reports failure, the policy records
//      REQUEST_FAILED and clears its duplicate memory, and no pin/residency is
//      disturbed.
//   5. Dirty-page eviction during shrink goes through the pool's PageLogSink
//      BEFORE the page reaches storage (the write-ahead barrier), and a sink
//      failure blocks the write and the resize.
//   6. Database-level integration: records inserted through the real
//      QueryExecutor survive a bridge-driven GROW and SHRINK, then survive
//      Close() and a recovery reopen with a smaller pool.
//   7. Dashboard engine metrics are filled from a REAL pool (capacity / resident
//      / pinned) while cache/latency remain honestly unavailable.
//
// Same plain PASS/FAIL style as the other tests; exit 0 on success, 1 otherwise.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bridge/adaptation_policy.h"
#include "bridge/buffer_pool_adapter.h"
#include "common/error.h"
#include "dashboard/dashboard.h"
#include "dashboard/metrics.h"
#include "db/buffer_pool.h"
#include "db/database.h"
#include "db/page.h"
#include "db/page_storage.h"
#include "db/query_executor.h"

namespace {

using nexusdb::BufferPool;
using nexusdb::Database;
using nexusdb::DbError;
using nexusdb::Page;
using nexusdb::PageLogSink;
using nexusdb::PageStorage;
using nexusdb::QueryExecutor;
using nexusdb::Status;

int g_failures = 0;

#define CHECK(desc, expr)                             \
  do {                                                \
    if (expr) {                                       \
      std::printf("PASS: %s\n", desc);                \
    } else {                                          \
      std::printf("FAIL: %s\n", desc);                \
      ++g_failures;                                   \
    }                                                 \
  } while (0)

constexpr unsigned long kMiB = 1024UL * 1024UL;

// A config suitable for driving a real pool: a 64 MiB limit gives a ceiling
// well above every pool size used here, so GROW is never clamped and SHRINK is
// the only path that exercises the hysteresis band.
void MakeGrowShrinkConfig(nexus_policy_config_t* cfg) {
  nexus_policy_config_default(cfg);
  cfg->pool_fraction = 1.0;
  cfg->min_frames = 16;
  cfg->max_frames = 0;
  cfg->shrink_threshold = 0.85;
  cfg->grow_threshold = 0.60;
  cfg->max_step_down = 256;
  cfg->max_step_up = 256;
  cfg->page_size = 4096;
}

nexus_resource_snapshot_t Snapshot(unsigned long current, unsigned long limit) {
  nexus_resource_snapshot_t s;
  std::memset(&s, 0, sizeof(s));
  s.valid = 1;
  s.has_memory = 1;
  s.memory_current = current;
  s.memory_max = limit;
  s.has_limit = (limit > 0) ? 1 : 0;
  s.memory_pressure =
      (limit > 0) ? static_cast<double>(current) / static_cast<double>(limit)
                  : 0.0;
  return s;
}

// Records every page that reaches the write-ahead barrier, and can be made to
// fail so the ordering guarantee can be asserted.
class RecordingSink : public PageLogSink {
 public:
  std::vector<Page::PageId> logged;
  bool fail_next = false;

  Status BeforePageWrite(const Page& page) override {
    if (fail_next) {
      return Status::Error(DbError::kIoError, "recording sink: forced failure");
    }
    logged.push_back(page.id());
    return Status::Ok();
  }
};

struct ProviderCtx {
  BufferPool* pool;
  unsigned long target;
};

int EngineMetricsProvider(void* ctx, nexus_engine_metrics_t* out) {
  auto* p = static_cast<ProviderCtx*>(ctx);
  if (p == nullptr || p->pool == nullptr || out == nullptr) {
    return NEXUS_ERR_INVALID;
  }
  std::memset(out, 0, sizeof(*out));
  out->has_pool = 1;
  out->pool_target_frames = p->target;
  out->pool_actual_frames = static_cast<unsigned long>(p->pool->capacity());
  out->pool_resident_frames = static_cast<unsigned long>(p->pool->size());
  out->pool_pinned_frames = static_cast<unsigned long>(p->pool->pinned_frames());
  // Cache counters / latency genuinely do not exist on the engine.
  return NEXUS_OK;
}

std::string TempDir() {
  return (std::filesystem::temp_directory_path() / "nexusdb_engine_adapt_test")
      .string();
}

std::string Join(const std::string& dir, const std::string& name) {
  return (std::filesystem::path(dir) / name).string();
}

void FillPattern(Page* page, unsigned char byte) {
  std::memset(page->data(), byte, Page::size());
  page->MarkDirty();
}

bool StorageMatches(PageStorage* storage, Page::PageId id, unsigned char byte) {
  Page p;
  if (!storage->ReadPage(id, &p).ok()) {
    return false;
  }
  for (std::size_t i = 0; i < Page::size(); ++i) {
    if (p.data()[i] != byte) {
      return false;
    }
  }
  return true;
}

}  // namespace

int main() {
  // ==================================================================
  // 1. Policy decision math at synthetic pressure levels (no engine).
  // ==================================================================
  {
    nexus_policy_config_t cfg;
    MakeGrowShrinkConfig(&cfg);
    CHECK("default config is valid", nexus_policy_config_valid(&cfg) == 1);

    nexus_policy_t policy;
    nexus_policy_init(&policy);
    nexus_adapt_decision_t d;

    // limit 64 MiB -> ceiling 16384 frames (fraction 1.0).
    nexus_resource_snapshot_t low = Snapshot(1 * kMiB, 64 * kMiB);
    nexus_resource_snapshot_t high = Snapshot(63 * kMiB, 64 * kMiB);

    int rc = nexus_policy_evaluate(&policy, &cfg, &low, 1000, &d);
    CHECK("low pressure -> GROW by bounded step",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_GROW &&
              d.target_frames == 1256);

    nexus_policy_init(&policy);
    rc = nexus_policy_evaluate(&policy, &cfg, &low, 1000, &d);  // GROW -> 1256
    rc = nexus_policy_evaluate(&policy, &cfg, &low, 1000, &d);  // same target
    CHECK("duplicate GROW target is suppressed",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_HOLD &&
              d.reason == NEXUS_REASON_DUPLICATE);

    nexus_policy_init(&policy);
    rc = nexus_policy_evaluate(&policy, &cfg, &high, 8000, &d);
    CHECK("high pressure -> SHRINK by bounded step",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_SHRINK &&
              d.target_frames == 7744);

    nexus_policy_init(&policy);
    nexus_resource_snapshot_t band = Snapshot(48 * kMiB, 64 * kMiB);  // 0.75
    rc = nexus_policy_evaluate(&policy, &cfg, &band, 1000, &d);
    CHECK("mid pressure -> HOLD within the hysteresis band",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_HOLD &&
              d.reason == NEXUS_REASON_WITHIN_BAND);

    nexus_policy_init(&policy);
    rc = nexus_policy_evaluate(&policy, &cfg, &high, cfg.min_frames, &d);
    CHECK("cannot shrink below the floor -> HOLD AT_MIN",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_HOLD &&
              d.reason == NEXUS_REASON_AT_MIN);

    nexus_policy_init(&policy);
    nexus_resource_snapshot_t unlimited = Snapshot(1 * kMiB, 0);
    rc = nexus_policy_evaluate(&policy, &cfg, &unlimited, 1000, &d);
    CHECK("unlimited memory -> HOLD UNLIMITED",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_HOLD &&
              d.reason == NEXUS_REASON_UNLIMITED);

    nexus_policy_init(&policy);
    nexus_resource_snapshot_t none;
    std::memset(&none, 0, sizeof(none));
    rc = nexus_policy_evaluate(&policy, &cfg, &none, 1000, &d);
    CHECK("missing metrics -> HOLD NO_METRICS (no fabricated pressure)",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_HOLD &&
              d.reason == NEXUS_REASON_NO_METRICS);

    nexus_policy_config_t bad = cfg;
    bad.grow_threshold = bad.shrink_threshold;  // invalid: not a band
    nexus_policy_init(&policy);
    rc = nexus_policy_evaluate(&policy, &bad, &low, 1000, &d);
    CHECK("invalid config is rejected",
          rc == NEXUS_ERR_INVALID && d.reason == NEXUS_REASON_INVALID_CONFIG);
  }

  const std::string dir = TempDir();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  // ==================================================================
  // 2 + 3. Adapter and policy drive a real BufferPool.
  // ==================================================================
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open storage",
          PageStorage::Open(Join(dir, "pool.db"), &storage).ok() &&
              storage != nullptr);

    for (int i = 0; i < 64; ++i) {
      Page tmp;
      if (!storage->AllocatePage(&tmp).ok()) {
        break;
      }
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create pool (capacity 64)",
          BufferPool::Create(storage.get(), 64, &pool).ok() && pool != nullptr);

    CHECK("adapter: null context -> NEXUS_ERR_INVALID",
          nexus_bridge_buffer_pool_request(nullptr, 32) == NEXUS_ERR_INVALID);
    CHECK("adapter: zero target -> NEXUS_ERR_INVALID",
          nexus_bridge_buffer_pool_request(pool.get(), 0) == NEXUS_ERR_INVALID);
    CHECK("adapter: invalid request left the pool unchanged",
          pool->capacity() == 64);

    CHECK("adapter: grow to 128 frames succeeds",
          nexus_bridge_buffer_pool_request(pool.get(), 128) == NEXUS_OK &&
              pool->capacity() == 128);
    CHECK("adapter: shrink to 32 frames succeeds",
          nexus_bridge_buffer_pool_request(pool.get(), 32) == NEXUS_OK &&
              pool->capacity() == 32);

    nexus_policy_config_t cfg;
    MakeGrowShrinkConfig(&cfg);
    nexus_policy_t policy;
    nexus_policy_init(&policy);

    nexus_resource_snapshot_t low = Snapshot(1 * kMiB, 64 * kMiB);
    nexus_adapt_decision_t d;
    int rc = nexus_policy_apply(&policy, &cfg, &low, pool->capacity(),
                                nexus_bridge_buffer_pool_request, pool.get(), &d);
    CHECK("policy GROW applied to the real pool",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_GROW &&
              pool->capacity() == d.target_frames);

    nexus_policy_init(&policy);
    nexus_resource_snapshot_t high = Snapshot(63 * kMiB, 64 * kMiB);
    unsigned long before = pool->capacity();
    rc = nexus_policy_apply(&policy, &cfg, &high, before,
                            nexus_bridge_buffer_pool_request, pool.get(), &d);
    CHECK("policy SHRINK applied to the real pool",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_SHRINK &&
              d.target_frames < before && pool->capacity() == d.target_frames);
  }

  // ==================================================================
  // 4. Pinned-frame shrink refusal is failure-atomic.
  // ==================================================================
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open pin storage",
          PageStorage::Open(Join(dir, "pin.db"), &storage).ok());
    for (int i = 0; i < 8; ++i) {
      Page tmp;
      if (!storage->AllocatePage(&tmp).ok()) {
        break;
      }
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create pool (capacity 8)",
          BufferPool::Create(storage.get(), 8, &pool).ok());

    std::vector<BufferPool::Handle> pins;
    bool pinned_all = true;
    for (Page::PageId id = 0; id < 8; ++id) {
      BufferPool::Handle h;
      if (!pool->Fetch(id, &h).ok()) {
        pinned_all = false;
        break;
      }
      pins.push_back(std::move(h));
    }
    CHECK("all 8 frames resident and pinned",
          pinned_all && pool->size() == 8 && pool->pinned_frames() == 8);

    CHECK("adapter: shrink below pinned count is refused",
          nexus_bridge_buffer_pool_request(pool.get(), 2) == NEXUS_ERR_PROCESS);
    CHECK("failed shrink left capacity and pins untouched",
          pool->capacity() == 8 && pool->size() == 8 &&
              pool->pinned_frames() == 8);

    nexus_policy_config_t cfg;
    MakeGrowShrinkConfig(&cfg);
    cfg.min_frames = 1;  // floor below the live capacity so a shrink is requested
    nexus_policy_t policy;
    nexus_policy_init(&policy);
    nexus_resource_snapshot_t high = Snapshot(63 * kMiB, 64 * kMiB);
    nexus_adapt_decision_t d;
    int rc = nexus_policy_apply(&policy, &cfg, &high, pool->capacity(),
                                nexus_bridge_buffer_pool_request, pool.get(), &d);
    CHECK("policy surfaces the refusal as REQUEST_FAILED",
          rc == NEXUS_ERR_PROCESS && d.reason == NEXUS_REASON_REQUEST_FAILED);
    CHECK("failed request clears duplicate memory so a retry can happen",
          policy.have_last == 0);

    bool handles_still_valid = true;
    for (auto& h : pins) {
      if (!h.valid()) {
        handles_still_valid = false;
      }
    }
    CHECK("no handle was invalidated by the failed shrink", handles_still_valid);

    pins.clear();  // unpin everything
    CHECK("shrink succeeds once the frames are unpinned",
          nexus_bridge_buffer_pool_request(pool.get(), 4) == NEXUS_OK &&
              pool->capacity() == 4);
  }

  // ==================================================================
  // 5. Dirty-page eviction during shrink honors the write-ahead barrier.
  // ==================================================================
  {
    std::unique_ptr<PageStorage> storage;
    CHECK("open evict storage",
          PageStorage::Open(Join(dir, "evict.db"), &storage).ok());
    for (int i = 0; i < 4; ++i) {
      Page tmp;
      if (!storage->AllocatePage(&tmp).ok()) {
        break;
      }
    }

    std::unique_ptr<BufferPool> pool;
    CHECK("create pool (capacity 4)",
          BufferPool::Create(storage.get(), 4, &pool).ok());

    RecordingSink sink;
    pool->SetLogSink(&sink);

    // Dirty all four resident pages (page 0 -> 0x10, page 1 -> 0x11, ...).
    for (Page::PageId id = 0; id < 4; ++id) {
      BufferPool::Handle h;
      if (!pool->Fetch(id, &h).ok()) {
        break;
      }
      FillPattern(&h.page(), static_cast<unsigned char>(0x10 + id));
      h.Unpin();
    }

    // Shrink to 2 evicts the two LRU (oldest) dirty pages.
    CHECK("dirty shrink to 2 frames succeeds",
          nexus_bridge_buffer_pool_request(pool.get(), 2) == NEXUS_OK &&
              pool->capacity() == 2);
    CHECK("barrier ran once per evicted dirty page", sink.logged.size() == 2);

    bool evicted_durable = true;
    for (Page::PageId id : sink.logged) {
      if (!StorageMatches(storage.get(), id,
                          static_cast<unsigned char>(0x10 + id))) {
        evicted_durable = false;
      }
    }
    CHECK("evicted dirty pages were written after the barrier", evicted_durable);

    // Now prove the barrier PRECEDES the storage write: a failing sink must
    // block the write, leave the victim dirty, and fail the resize atomically.
    RecordingSink failing;
    failing.fail_next = true;
    pool->SetLogSink(&failing);

    std::vector<Page::PageId> resident;
    for (Page::PageId id = 0; id < 4; ++id) {
      if (pool->Contains(id)) {
        resident.push_back(id);
      }
    }
    for (Page::PageId id : resident) {
      BufferPool::Handle h;
      if (!pool->Fetch(id, &h).ok()) {
        continue;
      }
      FillPattern(&h.page(), 0xEE);
      h.Unpin();
    }

    unsigned long cap_before = pool->capacity();
    CHECK("a failing barrier blocks the shrink",
          nexus_bridge_buffer_pool_request(pool.get(), 1) == NEXUS_ERR_PROCESS);
    CHECK("blocked shrink is atomic (capacity unchanged)",
          pool->capacity() == cap_before);

    bool not_written = true;
    for (Page::PageId id : resident) {
      if (StorageMatches(storage.get(), id, 0xEE)) {
        not_written = false;
      }
    }
    CHECK("blocked victim never reached storage (barrier precedes write)",
          not_written);

    pool->SetLogSink(nullptr);
  }

  // ==================================================================
  // 6. Database records survive a bridge-driven resize + WAL recovery.
  // ==================================================================
  const std::string db_path = Join(dir, "nexusdb.db");
  const std::string wal_path = Join(dir, "nexusdb.wal");
  constexpr std::size_t kRecordBytes = 1500;
  constexpr int kRecords = 300;

  Page::PageId catalog_root = Page::kInvalidId;
  {
    std::unique_ptr<Database> db;
    CHECK("open Database (pool 8)",
          Database::Open(db_path, wal_path, 8, &db).ok() && db != nullptr);
    if (db == nullptr) {
      std::printf("FATAL: cannot continue without a database\n");
      return 1;
    }

    CHECK("create table t", db->executor().CreateTable("t").ok());

    std::vector<uint8_t> buf(kRecordBytes);
    std::vector<QueryExecutor::RecordId> ids;
    bool inserted = true;
    for (int i = 0; i < kRecords; ++i) {
      std::memset(buf.data(), static_cast<unsigned char>(i % 251), kRecordBytes);
      QueryExecutor::RecordId id{};
      if (!db->executor().Insert("t", buf.data(), buf.size(), &id).ok()) {
        inserted = false;
        break;
      }
      ids.push_back(id);
    }
    CHECK("inserted all records", inserted && ids.size() == kRecords);

    auto records_correct = [&]() -> bool {
      for (int i = 0; i < kRecords; ++i) {
        const std::uint8_t* data = nullptr;
        std::size_t len = 0;
        if (!db->executor().Get("t", ids[i], &data, &len).ok()) {
          return false;
        }
        if (len != kRecordBytes) {
          return false;
        }
        const unsigned char want = static_cast<unsigned char>(i % 251);
        for (std::size_t b = 0; b < len; ++b) {
          if (data[b] != want) {
            return false;
          }
        }
      }
      return true;
    };

    CHECK("records correct before any resize", records_correct());

    // Bridge -> policy -> adapter GROW on the live database.
    nexus_policy_config_t cfg;
    MakeGrowShrinkConfig(&cfg);
    nexus_policy_t policy;
    nexus_policy_init(&policy);
    nexus_adapt_decision_t d;
    nexus_resource_snapshot_t low = Snapshot(1 * kMiB, 64 * kMiB);
    int rc = nexus_policy_apply(&policy, &cfg, &low, db->pool()->capacity(),
                                nexus_bridge_buffer_pool_request, db->pool(), &d);
    CHECK("bridge-driven GROW resized the database pool",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_GROW &&
              db->pool()->capacity() == d.target_frames);
    CHECK("records correct after GROW", records_correct());

    // Bridge -> policy -> adapter SHRINK on the live database.
    nexus_policy_init(&policy);
    nexus_resource_snapshot_t high = Snapshot(63 * kMiB, 64 * kMiB);
    unsigned long pre_shrink = db->pool()->capacity();
    rc = nexus_policy_apply(&policy, &cfg, &high, pre_shrink,
                            nexus_bridge_buffer_pool_request, db->pool(), &d);
    CHECK("bridge-driven SHRINK resized the database pool",
          rc == NEXUS_OK && d.action == NEXUS_ADAPT_SHRINK &&
              db->pool()->capacity() == d.target_frames &&
              db->pool()->capacity() < pre_shrink);
    CHECK("records correct after SHRINK", records_correct());

    catalog_root = db->catalog_root();
    CHECK("catalog root recorded", catalog_root != Page::kInvalidId);
    CHECK("Close() succeeds", db->Close().ok());
  }

  // Reopen with a different (smaller) pool: recovery must restore every record.
  {
    std::unique_ptr<Database> db;
    CHECK("reopen Database (recovery, pool 4)",
          Database::Open(db_path, wal_path, 4, &db).ok() && db != nullptr);
    if (db != nullptr) {
      CHECK("catalog root survived reopen", db->catalog_root() == catalog_root);
      CHECK("table t survived reopen", db->executor().TableExists("t"));

      struct ScanCtx {
        int count = 0;
        bool ok = true;
      } sc;
      auto fn = +[](const QueryExecutor::RecordId&, const std::uint8_t* data,
                    std::size_t len, void* ctx) -> bool {
        auto* c = static_cast<ScanCtx*>(ctx);
        ++c->count;
        if (len != kRecordBytes) {
          c->ok = false;
        }
        // Every byte in a record is identical; spot-check first and last.
        if (len > 0 && data[0] != data[len - 1]) {
          c->ok = false;
        }
        return true;
      };
      CHECK("scan recovered table",
            db->executor().Scan("t", fn, &sc).ok());
      CHECK("all records recovered after reopen",
            sc.ok && sc.count == kRecords);
      CHECK("Close() after recovery succeeds", db->Close().ok());
    }
  }

  // ==================================================================
  // 7. Dashboard engine metrics from a REAL pool; unavailable is honest.
  // ==================================================================
  {
    std::unique_ptr<PageStorage> storage;
    if (PageStorage::Open(Join(dir, "dash.db"), &storage).ok()) {
      for (int i = 0; i < 4; ++i) {
        Page tmp;
        if (!storage->AllocatePage(&tmp).ok()) {
          break;
        }
      }
      std::unique_ptr<BufferPool> pool;
      if (BufferPool::Create(storage.get(), 4, &pool).ok()) {
        ProviderCtx pctx;
        pctx.pool = pool.get();
        pctx.target = 6;

        nexus_dashboard_inputs_t in;
        std::memset(&in, 0, sizeof(in));
        nexus_dashboard_metrics_t m;
        int rc = nexus_dashboard_collect(&in, EngineMetricsProvider, &pctx, &m);
        CHECK("dashboard collected real engine pool metrics",
              rc == NEXUS_OK && m.engine.has_pool == 1 &&
                  m.engine.pool_actual_frames == 4 &&
                  m.engine.pool_target_frames == 6);
        CHECK("dashboard keeps cache stats honestly unavailable",
              m.engine.has_cache_stats == 0 && m.engine.has_latency == 0);

        char text[2048];
        CHECK("dashboard renders",
              nexus_dashboard_render_text(&m, text, sizeof(text)) == NEXUS_OK);
        const std::string rendered(text);
        CHECK("rendered text shows the real pool capacity",
              rendered.find("Buffer actual:    4 frames") != std::string::npos);
        CHECK("rendered text labels cache as unavailable",
              rendered.find("Cache hits:       unavailable") !=
                  std::string::npos);
      }
    }
  }

  std::filesystem::remove_all(dir, ec);

  std::printf("\n");
  if (g_failures == 0) {
    std::printf("=== ALL TESTS PASSED (0 failure(s)) ===\n");
    return 0;
  }
  std::printf("=== %d FAILURE(S) ===\n", g_failures);
  return 1;
}
