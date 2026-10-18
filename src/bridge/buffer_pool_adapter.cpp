/*
 * NexusDB — OS–DB Bridge ↔ Database Engine adapter implementation (Rehan).
 * See include/bridge/buffer_pool_adapter.h for the contract and the honest
 * build limitation note.
 */
#include "bridge/buffer_pool_adapter.h"

#include <cstddef>

#include "common/error.h"
#include "db/buffer_pool.h"

extern "C" int nexus_bridge_buffer_pool_request(void *context,
                                                unsigned long target_frames)
{
    nexusdb::BufferPool *pool =
        static_cast<nexusdb::BufferPool *>(context);

    if (pool == nullptr || target_frames == 0)
        return NEXUS_ERR_INVALID;

    const nexusdb::Status status =
        pool->Resize(static_cast<std::size_t>(target_frames));
    return status.ok() ? NEXUS_OK : NEXUS_ERR_PROCESS;
}
