/*
 * NexusDB — OS–DB Bridge ↔ Database Engine adapter (Rehan: integration).
 *
 * Thin shim that lets the bridge's adaptation policy request a resize through
 * the database engine's PUBLIC BufferPool API. It lives on the bridge side,
 * uses only Digivijay's public interface, and changes no engine code.
 *
 * This translation unit is meant to be compiled with the C++ engine toolchain
 * (GCC 11+ / C++17). On the current dev machine the C++ engine does not build,
 * so this file is only compile-checked there, not linked; the end-to-end
 * pinned/dirty integration test runs on Linux/WSL2.
 */
#ifndef NEXUS_BRIDGE_BUFFER_POOL_ADAPTER_H
#define NEXUS_BRIDGE_BUFFER_POOL_ADAPTER_H

#include "bridge/adaptation_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A nexus_resize_request_fn adapter. `context` must point at a
 * nexusdb::BufferPool. Resizes the pool to `target_frames` pages and maps the
 * engine Status onto the repository's bridge codes:
 *   NEXUS_OK          resize succeeded
 *   NEXUS_ERR_INVALID context is NULL, or target_frames == 0
 *   NEXUS_ERR_PROCESS Resize failed (kBufferFull/kOutOfMemory/I-O/...)
 *
 * The engine's Resize() is itself failure-atomic and preserves pinned and
 * dirty frames, so the policy never has to reason about those invariants.
 */
int nexus_bridge_buffer_pool_request(void *context, unsigned long target_frames);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_BRIDGE_BUFFER_POOL_ADAPTER_H */
