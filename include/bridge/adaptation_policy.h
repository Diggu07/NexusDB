/*
 * NexusDB — OS–DB Bridge: memory-pressure adaptation policy (Rehan).
 *
 * Turns a real cgroup resource snapshot into a *requested* buffer-pool
 * capacity. It is deliberately engine-agnostic: it computes a target and
 * hands it to the caller through a resize-request callback, so the policy can
 * be unit-tested without a database or root, and the database engine stays
 * the only component that mutates its own buffer pool.
 *
 * Decision rule (memory pressure drives the pool; CPU is never used):
 *   pressure >= shrink_threshold        -> SHRINK by up to max_step_down
 *   pressure <= grow_threshold          -> GROW   by up to max_step_up
 *   in between                          -> HOLD   (hysteresis dead-band)
 *
 * Safety properties:
 *   - No metrics / no memory reading -> HOLD (never a fabricated pressure).
 *   - Unlimited memory ("max")       -> HOLD (there is no pressure to respond to).
 *   - The pool may use at most `pool_fraction` of the memory limit; the limit
 *     is cgroup-total memory, so the pool must NOT assume it may take it all.
 *   - Capacity is clamped to [min_frames, max_frames] and each step is bounded.
 *   - A target equal to the last issued request is suppressed (no repeat spam);
 *     a failed application resets that memory so a retry is possible.
 *
 * All fallible calls return the repository's NEXUS_* status codes; no
 * exception and no hidden allocation crosses this C API.
 */
#ifndef NEXUS_BRIDGE_ADAPTATION_POLICY_H
#define NEXUS_BRIDGE_ADAPTATION_POLICY_H

#include "bridge/resource_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tunables. All values are explicit so behaviour is diagnosable/reproducible. */
typedef struct nexus_policy_config {
    double shrink_threshold;   /* pressure at/above this -> shrink (e.g. 0.85) */
    double grow_threshold;     /* pressure at/below this -> grow   (e.g. 0.60) */
    double pool_fraction;      /* max share of the limit the pool may use (0,1] */
    unsigned long page_size;   /* bytes per frame; matches the engine's kPageSize */
    unsigned long min_frames;  /* hard floor on pool capacity (>= 1) */
    unsigned long max_frames;  /* hard ceiling; 0 = no explicit ceiling */
    unsigned long max_step_down; /* most frames removed in one decision */
    unsigned long max_step_up;   /* most frames added in one decision */
} nexus_policy_config_t;

typedef enum nexus_adapt_action {
    NEXUS_ADAPT_HOLD = 0,
    NEXUS_ADAPT_GROW = 1,
    NEXUS_ADAPT_SHRINK = 2
} nexus_adapt_action_t;

typedef enum nexus_adapt_reason {
    NEXUS_REASON_OK = 0,          /* a decision was produced */
    NEXUS_REASON_NO_METRICS,      /* snapshot invalid / memory unavailable */
    NEXUS_REASON_UNLIMITED,       /* memory.max is "max": no pressure signal */
    NEXUS_REASON_WITHIN_BAND,     /* between thresholds: hysteresis hold */
    NEXUS_REASON_AT_MIN,          /* cannot shrink below min_frames */
    NEXUS_REASON_AT_MAX,          /* cannot grow above the ceiling */
    NEXUS_REASON_DUPLICATE,       /* target equals the last issued request */
    NEXUS_REASON_REQUEST_FAILED,  /* the resize-request callback failed */
    NEXUS_REASON_INVALID_CONFIG   /* config failed validation */
} nexus_adapt_reason_t;

typedef struct nexus_adapt_decision {
    nexus_adapt_action_t action;
    unsigned long target_frames;  /* requested capacity (== current on HOLD) */
    double pressure;              /* pressure the decision was based on */
    nexus_adapt_reason_t reason;  /* why this action was chosen */
} nexus_adapt_decision_t;

/*
 * Caller-owned policy state. Holds the last issued target so identical
 * requests are not spammed. Re-initialize with nexus_policy_init() to forget.
 */
typedef struct nexus_policy {
    unsigned long last_target;
    int have_last;
} nexus_policy_t;

/*
 * The integration boundary (requirement 12): the bridge asks the database
 * engine to make its buffer pool `target_frames` pages. `context` is opaque to
 * the policy; the engine-side adapter supplies it. Must return NEXUS_OK on
 * success, any other NEXUS_* code on failure (surfaced as REQUEST_FAILED).
 */
typedef int (*nexus_resize_request_fn)(void *context, unsigned long target_frames);

/* Fill `cfg` with the documented defaults (shrink 0.85, grow 0.60, fraction
 * 0.6, page 4096, min 16, no ceiling, step 256). No-op on NULL. */
void nexus_policy_config_default(nexus_policy_config_t *cfg);

/* Reset policy state (forgets the last target). No-op on NULL. */
void nexus_policy_init(nexus_policy_t *policy);

/* 1 when `cfg` is non-NULL and every field is within a sane range. */
int nexus_policy_config_valid(const nexus_policy_config_t *cfg);

/*
 * Evaluate a snapshot and produce a decision without applying it. On a GROW/
 * SHRINK decision the policy's last-target memory is updated. Returns
 * NEXUS_ERR_INVALID for NULL args or an invalid config (and then `out` is
 * filled with a HOLD/INVALID_CONFIG decision when `out` is non-NULL).
 */
int nexus_policy_evaluate(nexus_policy_t *policy,
                          const nexus_policy_config_t *cfg,
                          const nexus_resource_snapshot_t *snapshot,
                          unsigned long current_frames,
                          nexus_adapt_decision_t *out);

/*
 * Evaluate and, when the decision is GROW/SHRINK, invoke `request` to ask the
 * engine to resize. HOLD decisions never call `request`. On a callback failure
 * the code is returned, `out->reason` becomes REQUEST_FAILED, and the
 * last-target memory is cleared so the request can be retried.
 */
int nexus_policy_apply(nexus_policy_t *policy,
                       const nexus_policy_config_t *cfg,
                       const nexus_resource_snapshot_t *snapshot,
                       unsigned long current_frames,
                       nexus_resize_request_fn request,
                       void *context,
                       nexus_adapt_decision_t *out);

const char *nexus_adapt_action_str(nexus_adapt_action_t action);
const char *nexus_adapt_reason_str(nexus_adapt_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_BRIDGE_ADAPTATION_POLICY_H */
