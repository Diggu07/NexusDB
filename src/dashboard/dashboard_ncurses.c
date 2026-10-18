/*
 * NexusDB — Live dashboard: ncurses renderer (Rehan).
 *
 * Compiled ONLY when NEXUS_HAVE_NCURSES is defined (CMake find_package(Curses)
 * / Makefile -DNEXUS_HAVE_NCURSES). On the current dev machine ncurses is not
 * installed, so this file is NOT compiled and its behaviour is UNVERIFIED;
 * it must be built and run on Linux/WSL2 with libncurses-dev before the live
 * dashboard is claimed to work.
 *
 * Presents the same text as nexus_dashboard_render_text(), so the plain
 * renderer remains the single source of the frame contents and is unit-tested
 * independently.
 */
#include "dashboard/dashboard.h"

#include <string.h>

#ifdef NEXUS_HAVE_NCURSES

#include <ncurses.h>

/* Draw one frame by reusing the tested text renderer. Splits on '\n' without
 * strtok (portability) and clips to the current terminal size, so a resize is
 * handled implicitly on the next erase()/refresh(). */
static void draw_frame(const nexus_dashboard_metrics_t *metrics, int refresh_ms)
{
    char text[2048];
    char *p = text;
    int row = 0;

    if (nexus_dashboard_render_text(metrics, text, sizeof(text)) != NEXUS_OK)
        snprintf(text, sizeof(text), "render error (buffer too small)");

    erase();
    while (*p != '\0' && row < LINES - 1) {
        char *nl = strchr(p, '\n');
        if (nl != NULL)
            *nl = '\0';
        mvprintw(row++, 0, "%s", p);
        if (nl == NULL)
            break;
        p = nl + 1;
    }
    mvprintw(LINES - 1, 0, "q: quit   refresh: %d ms", refresh_ms);
    refresh();
}

int nexus_dashboard_run_ncurses(nexus_dashboard_sample_fn sample,
                                void *context,
                                int refresh_ms,
                                unsigned max_iterations)
{
    int rc = NEXUS_OK;
    unsigned iterations = 0;

    if (sample == NULL)
        return NEXUS_ERR_INVALID;
    if (refresh_ms < 0)
        refresh_ms = 0;

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(refresh_ms); /* getch blocks at most refresh_ms: no busy-wait */

    for (;;) {
        nexus_dashboard_metrics_t metrics;
        int ch;

        if (sample(context, &metrics) != NEXUS_OK) {
            rc = NEXUS_ERR_PROCESS;
            break;
        }

        draw_frame(&metrics, refresh_ms);

        ch = getch();
        if (ch == 'q' || ch == 'Q')
            break; /* KEY_RESIZE and timeouts simply redraw next iteration */

        iterations++;
        if (max_iterations != 0 && iterations >= max_iterations)
            break;
    }

    endwin(); /* always restore the terminal */
    return rc;
}

#else /* !NEXUS_HAVE_NCURSES */

/* Not built by default. If this file is compiled without ncurses, provide an
 * honest stub so a misconfigured build fails loudly at runtime instead of
 * silently doing nothing. */
int nexus_dashboard_run_ncurses(nexus_dashboard_sample_fn sample,
                                void *context,
                                int refresh_ms,
                                unsigned max_iterations)
{
    (void)sample;
    (void)context;
    (void)refresh_ms;
    (void)max_iterations;
    return NEXUS_ERR_STATE;
}

#endif /* NEXUS_HAVE_NCURSES */
