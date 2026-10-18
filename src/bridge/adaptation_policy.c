/*
 * NexusDB — OS–DB Bridge: memory-pressure adaptation policy implementation.
 * See include/bridge/adaptation_policy.h for the contract.
 *
 * Pure computation over a snapshot: no I/O, no allocation, no engine calls.
 * The only side effect is the small caller-owned last-target memory used to
 * suppress duplicate requests.
 */
#include "bridge/adaptation_policy.h"

#include <limits.h>
#include <string.h>

void nexus_policy_config_default(nexus_policy_config_t *cfg)
{
    if (cfg == NULL)
        return;
    cfg->shrink_threshold = 0.85;
    cfg->grow_threshold = 0.60;
    cfg->pool_fraction = 0.6;
    cfg->page_size = 4096;
    cfg->min_frames = 16;
    cfg->max_frames = 0; /* no explicit ceiling */
    cfg->max_step_down = 256;
    cfg->max_step_up = 256;
}

void nexus_policy_init(nexus_policy_t *policy)
{
    if (policy == NULL)
        return;
    policy->last_target = 0;
    policy->have_last = 0;
}

int nexus_policy_config_valid(const nexus_policy_config_t *cfg)
{
    if (cfg == NULL)
        return 0;
    if (!(cfg->shrink_threshold > 0.0))
        return 0;
    if (cfg->grow_threshold < 0.0)
        return 0;
    if (cfg->grow_threshold >= cfg->shrink_threshold)
        return 0;
    if (!(cfg->pool_fraction > 0.0 && cfg->pool_fraction <= 1.0))
        return 0;
    if (cfg->page_size == 0)
        return 0;
    if (cfg->min_frames == 0)
        return 0;
    if (cfg->max_frames != 0 && cfg->max_frames < cfg->min_frames)
        return 0;
    if (cfg->max_step_down == 0 && cfg->max_step_up == 0)
        return 0;
    return 1;
}

static unsigned long sub_floor(unsigned long a, unsigned long b)
{
    return a > b ? a - b : 0;
}

static unsigned long add_sat(unsigned long a, unsigned long b)
{
    if (a > ULONG_MAX - b)
        return ULONG_MAX;
    return a + b;
}

/* Frames the cgroup's memory limit allows the pool to occupy: pool_fraction
 * of the limit, floored at min_frames and capped by max_frames. */
static unsigned long ceiling_frames(const nexus_policy_config_t *cfg,
                                    unsigned long limit_bytes)
{
    double limit_frames = (double)limit_bytes / (double)cfg->page_size;
    double ceiling_d = limit_frames * cfg->pool_fraction;
    unsigned long ceiling;

    if (ceiling_d >= (double)ULONG_MAX)
        ceiling = ULONG_MAX;
    else
        ceiling = (unsigned long)ceiling_d;

    if (ceiling < cfg->min_frames)
        ceiling = cfg->min_frames;
    if (cfg->max_frames != 0 && ceiling > cfg->max_frames)
        ceiling = cfg->max_frames;
    return ceiling;
}

int nexus_policy_evaluate(nexus_policy_t *policy,
                          const nexus_policy_config_t *cfg,
                          const nexus_resource_snapshot_t *snapshot,
                          unsigned long current_frames,
                          nexus_adapt_decision_t *out)
{
    unsigned long ceiling;
    unsigned long target;
    double pressure;

    if (policy == NULL || cfg == NULL || out == NULL)
        return NEXUS_ERR_INVALID;

    out->action = NEXUS_ADAPT_HOLD;
    out->target_frames = current_frames;
    out->pressure = 0.0;
    out->reason = NEXUS_REASON_OK;

    if (!nexus_policy_config_valid(cfg)) {
        out->reason = NEXUS_REASON_INVALID_CONFIG;
        return NEXUS_ERR_INVALID;
    }

    if (snapshot == NULL || !snapshot->valid || !snapshot->has_memory) {
        out->reason = NEXUS_REASON_NO_METRICS;
        return NEXUS_OK;
    }

    pressure = snapshot->memory_pressure;
    if (pressure < 0.0)
        pressure = 0.0;
    out->pressure = pressure;

    if (!snapshot->has_limit) {
        out->reason = NEXUS_REASON_UNLIMITED;
        return NEXUS_OK;
    }

    ceiling = ceiling_frames(cfg, snapshot->memory_max);

    if (pressure >= cfg->shrink_threshold) {
        target = sub_floor(current_frames, cfg->max_step_down);
        if (target < cfg->min_frames)
            target = cfg->min_frames;
        if (target >= current_frames) {
            out->reason = NEXUS_REASON_AT_MIN;
            return NEXUS_OK;
        }
        if (policy->have_last && policy->last_target == target) {
            out->reason = NEXUS_REASON_DUPLICATE;
            return NEXUS_OK;
        }
        policy->last_target = target;
        policy->have_last = 1;
        out->action = NEXUS_ADAPT_SHRINK;
        out->target_frames = target;
        return NEXUS_OK;
    }

    if (pressure <= cfg->grow_threshold) {
        if (current_frames >= ceiling) {
            out->reason = NEXUS_REASON_AT_MAX;
            return NEXUS_OK;
        }
        target = add_sat(current_frames, cfg->max_step_up);
        if (target > ceiling)
            target = ceiling;
        if (target <= current_frames) {
            out->reason = NEXUS_REASON_AT_MAX;
            return NEXUS_OK;
        }
        if (policy->have_last && policy->last_target == target) {
            out->reason = NEXUS_REASON_DUPLICATE;
            return NEXUS_OK;
        }
        policy->last_target = target;
        policy->have_last = 1;
        out->action = NEXUS_ADAPT_GROW;
        out->target_frames = target;
        return NEXUS_OK;
    }

    out->reason = NEXUS_REASON_WITHIN_BAND;
    return NEXUS_OK;
}

int nexus_policy_apply(nexus_policy_t *policy,
                       const nexus_policy_config_t *cfg,
                       const nexus_resource_snapshot_t *snapshot,
                       unsigned long current_frames,
                       nexus_resize_request_fn request,
                       void *context,
                       nexus_adapt_decision_t *out)
{
    int rc;

    rc = nexus_policy_evaluate(policy, cfg, snapshot, current_frames, out);
    if (rc != NEXUS_OK)
        return rc;

    if (out->action == NEXUS_ADAPT_HOLD)
        return NEXUS_OK;

    if (request == NULL)
        return NEXUS_ERR_INVALID;

    rc = request(context, out->target_frames);
    if (rc != NEXUS_OK) {
        out->reason = NEXUS_REASON_REQUEST_FAILED;
        /* Allow a retry: the failed target was not actually applied. */
        if (policy != NULL) {
            policy->last_target = 0;
            policy->have_last = 0;
        }
        return rc;
    }
    return NEXUS_OK;
}

const char *nexus_adapt_action_str(nexus_adapt_action_t action)
{
    switch (action) {
    case NEXUS_ADAPT_HOLD:   return "HOLD";
    case NEXUS_ADAPT_GROW:   return "GROW";
    case NEXUS_ADAPT_SHRINK: return "SHRINK";
    default:                 return "UNKNOWN";
    }
}

const char *nexus_adapt_reason_str(nexus_adapt_reason_t reason)
{
    switch (reason) {
    case NEXUS_REASON_OK:             return "ok";
    case NEXUS_REASON_NO_METRICS:     return "no metrics";
    case NEXUS_REASON_UNLIMITED:      return "unlimited memory";
    case NEXUS_REASON_WITHIN_BAND:    return "within hysteresis band";
    case NEXUS_REASON_AT_MIN:         return "at minimum capacity";
    case NEXUS_REASON_AT_MAX:         return "at maximum capacity";
    case NEXUS_REASON_DUPLICATE:      return "duplicate request suppressed";
    case NEXUS_REASON_REQUEST_FAILED: return "resize request failed";
    case NEXUS_REASON_INVALID_CONFIG: return "invalid configuration";
    default:                          return "unknown";
    }
}
