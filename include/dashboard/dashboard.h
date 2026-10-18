/*
 * NexusDB — Live dashboard: presentation (Rehan: Bridge & Observability).
 *
 * Presentation is split from collection (dashboard/metrics.h) so it can be
 * tested without a terminal.
 *
 * Two renderers:
 *   - nexus_dashboard_render_text(): always built, plain text into a buffer.
 *     This is the default/fallback and is fully unit-tested.
 *   - nexus_dashboard_run_ncurses(): the live ncurses UI, compiled ONLY when
 *     NEXUS_HAVE_NCURSES is defined. On this dev machine ncurses is not
 *     installed, so it is NOT compiled here and is UNVERIFIED until built on
 *     Linux/WSL2 with libncurses-dev.
 *
 * The ncurses loop restores terminal state (endwin), handles KEY_RESIZE, polls
 * at a controlled interval (no busy-wait), and exits on 'q' or a sample error.
 */
#ifndef NEXUS_DASHBOARD_DASHBOARD_H
#define NEXUS_DASHBOARD_DASHBOARD_H

#include <stddef.h>

#include "dashboard/metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Render one dashboard frame as plain text into `buf` (NUL-terminated).
 * Every unavailable metric is printed as "unavailable" (never a fake zero).
 *
 * Returns NEXUS_OK on success, NEXUS_ERR_INVALID for a NULL buffer / zero
 * length / NULL metrics, or NEXUS_ERR_PROCESS when `buf` was too small and the
 * output was truncated.
 */
int nexus_dashboard_render_text(const nexus_dashboard_metrics_t *metrics,
                                char *buf, size_t len);

/* Called by the ncurses loop to obtain each frame. */
typedef int (*nexus_dashboard_sample_fn)(void *context,
                                         nexus_dashboard_metrics_t *out);

/*
 * Live ncurses dashboard loop. `refresh_ms` is the poll interval; pass
 * `max_iterations == 0` to run until the user quits. Returns NEXUS_OK on a
 * clean exit or an error code. Only available when built with
 * NEXUS_HAVE_NCURSES; otherwise this symbol is not provided.
 */
int nexus_dashboard_run_ncurses(nexus_dashboard_sample_fn sample,
                                void *context,
                                int refresh_ms,
                                unsigned max_iterations);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_DASHBOARD_DASHBOARD_H */
