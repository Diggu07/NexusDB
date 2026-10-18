/*
 * NexusDB — Benchmark harness: database-engine workload provider (Rehan).
 *
 * The real workload behind the harness: a BufferPool over a PageStorage, driven
 * through the engine's public API only. Provides the nexus_bench_workload_t
 * callbacks declared in include/benchmark/harness.h.
 *
 * Build requirement: the C++ DB engine (Digvijay), GCC 11+ / C++17, on Linux.
 * It is intentionally NOT built on the Windows dev machine, where the engine's
 * <filesystem> use does not compile; the benchmark refuses to run there rather
 * than substitute synthetic data.
 *
 * Honest metrics:
 *   - cache hit/miss is decided by BufferPool::Contains(id) BEFORE Fetch, so it
 *     reflects real residency, not an engine counter (the engine has none).
 *   - resize goes through nexus_bridge_buffer_pool_request(), the same adapter
 *     the adaptation policy uses, so it exercises the production path.
 */
#include "benchmark/harness.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "bridge/buffer_pool_adapter.h"
#include "common/error.h"
#include "db/buffer_pool.h"
#include "db/page.h"
#include "db/page_storage.h"

namespace {

struct EngineWorkload {
    std::unique_ptr<nexusdb::PageStorage> storage;
    std::unique_ptr<nexusdb::BufferPool> pool;
    unsigned long page_count = 0;
    unsigned long dirty_every = 0;
    std::uint64_t rng = 1;
    int random = 0;
};

std::uint64_t NextRand(std::uint64_t *state)
{
    std::uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

}  // namespace

extern "C" {

static int bench_engine_open(void *ctx, const nexus_bench_config_t *cfg)
{
    EngineWorkload *w = static_cast<EngineWorkload *>(ctx);

    if (w == nullptr || cfg == nullptr)
        return NEXUS_ERR_INVALID;

    w->page_count = (cfg->page_count == 0) ? 1 : cfg->page_count;
    w->dirty_every = cfg->dirty_every;
    w->random = cfg->random;
    w->rng = (cfg->random_seed != 0) ? cfg->random_seed : 1;

    const char *path =
        (cfg->db_path != nullptr && cfg->db_path[0] != '\0')
            ? cfg->db_path
            : "data/benchmark.db";

    std::unique_ptr<nexusdb::PageStorage> storage;
    nexusdb::Status st = nexusdb::PageStorage::Open(path, &storage);
    if (!st.ok())
        return NEXUS_ERR_PROCESS;

    /* Ensure the file holds at least page_count dense pages. An existing file
     * with more pages is left intact (ids are stable and never reused). */
    std::uint32_t have = 0;
    st = storage->NumPages(&have);
    if (!st.ok())
        return NEXUS_ERR_PROCESS;
    nexusdb::Page tmp;
    while (have < w->page_count) {
        st = storage->AllocatePage(&tmp);
        if (!st.ok())
            return NEXUS_ERR_PROCESS;
        have++;
    }

    std::unique_ptr<nexusdb::BufferPool> pool;
    const std::size_t frames =
        (cfg->pool_frames_start == 0) ? 1 : cfg->pool_frames_start;
    st = nexusdb::BufferPool::Create(storage.get(), frames, &pool);
    if (!st.ok())
        return NEXUS_ERR_PROCESS;

    w->storage = std::move(storage);
    w->pool = std::move(pool);
    return NEXUS_OK;
}

static int bench_engine_step(void *ctx, unsigned long index, int *cache_hit)
{
    EngineWorkload *w = static_cast<EngineWorkload *>(ctx);

    if (w == nullptr || w->pool == nullptr || cache_hit == nullptr)
        return NEXUS_ERR_STATE;

    std::uint32_t id;
    if (w->random != 0)
        id = static_cast<std::uint32_t>(NextRand(&w->rng) % w->page_count);
    else
        id = static_cast<std::uint32_t>(index % w->page_count);

    /* Harness-observed hit: the page was already resident before the Fetch. */
    *cache_hit = w->pool->Contains(id) ? 1 : 0;

    nexusdb::BufferPool::Handle handle;
    const nexusdb::Status st = w->pool->Fetch(id, &handle);
    if (!st.ok())
        return NEXUS_ERR_PROCESS;

    if (w->dirty_every != 0 && (index % w->dirty_every) == 0)
        handle.MarkDirty();

    return NEXUS_OK; /* handle unpins at end of scope */
}

static unsigned long bench_engine_capacity(void *ctx)
{
    EngineWorkload *w = static_cast<EngineWorkload *>(ctx);
    return (w != nullptr && w->pool != nullptr)
               ? static_cast<unsigned long>(w->pool->capacity())
               : 0;
}

static unsigned long bench_engine_resident(void *ctx)
{
    EngineWorkload *w = static_cast<EngineWorkload *>(ctx);
    return (w != nullptr && w->pool != nullptr)
               ? static_cast<unsigned long>(w->pool->size())
               : 0;
}

static unsigned long bench_engine_pinned(void *ctx)
{
    EngineWorkload *w = static_cast<EngineWorkload *>(ctx);
    return (w != nullptr && w->pool != nullptr)
               ? static_cast<unsigned long>(w->pool->pinned_frames())
               : 0;
}

static int bench_engine_resize(void *ctx, unsigned long target_frames)
{
    EngineWorkload *w = static_cast<EngineWorkload *>(ctx);

    if (w == nullptr || w->pool == nullptr)
        return NEXUS_ERR_STATE;
    /* Reuse the production adapter so the benchmark hits the same code path. */
    return nexus_bridge_buffer_pool_request(w->pool.get(), target_frames);
}

static void bench_engine_close(void *ctx)
{
    EngineWorkload *w = static_cast<EngineWorkload *>(ctx);

    if (w == nullptr)
        return;
    if (w->pool != nullptr) {
        (void)w->pool->FlushAll();
        w->pool.reset();
    }
    if (w->storage != nullptr) {
        (void)w->storage->Close();
        w->storage.reset();
    }
}

int nexus_bench_engine_workload(nexus_bench_workload_t *out)
{
    /* One scenario per process: a single engine instance is sufficient and
     * keeps the callback context lifetime trivial. */
    static EngineWorkload instance;

    if (out == nullptr)
        return NEXUS_ERR_INVALID;

    out->ctx = &instance;
    out->open = bench_engine_open;
    out->step = bench_engine_step;
    out->capacity = bench_engine_capacity;
    out->resident = bench_engine_resident;
    out->pinned = bench_engine_pinned;
    out->resize = bench_engine_resize;
    out->close = bench_engine_close;
    return NEXUS_OK;
}

}  /* extern "C" */
