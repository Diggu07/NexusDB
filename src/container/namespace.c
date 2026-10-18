/*
 * NexusDB — Container Engine: Linux Namespace Isolation implementation.
 *
 * Real target: Ubuntu 22.04 / WSL2 using unshare() / clone().
 * Dev target: Windows fallback shim.
 */
/* _GNU_SOURCE must precede every system header: sched.h (unshare/CLONE_*),
 * unistd.h (sethostname) and sys/mount.h (mount/MS_*) are all gated on it
 * under -std=c11. Defining it after <string.h> (as before) had no effect. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "container/namespace.h"
#include "container/container.h"

#include <string.h>

#ifndef _WIN32
#include <sched.h>
#include <unistd.h>
#include <sys/mount.h>
#endif

void namespace_config_default(nexus_namespace_config_t *cfg, const char *container_name)
{
    if (!cfg)
        return;
    cfg->isolate_pid = 1;
    cfg->isolate_mount = 1;
    cfg->isolate_net = 1;
    cfg->isolate_uts = 1;
    cfg->isolate_ipc = 1;
    cfg->hostname = container_name;
}

int namespace_clone_flags(const nexus_namespace_config_t *cfg)
{
    if (cfg == NULL)
        return 0;

#ifdef _WIN32
    return 0; /* namespaces are Linux-only */
#else
    {
        int flags = 0;
#ifdef CLONE_NEWPID
        if (cfg->isolate_pid)   flags |= CLONE_NEWPID;
#endif
#ifdef CLONE_NEWNS
        if (cfg->isolate_mount) flags |= CLONE_NEWNS;
#endif
#ifdef CLONE_NEWNET
        if (cfg->isolate_net)   flags |= CLONE_NEWNET;
#endif
#ifdef CLONE_NEWUTS
        if (cfg->isolate_uts)   flags |= CLONE_NEWUTS;
#endif
#ifdef CLONE_NEWIPC
        if (cfg->isolate_ipc)   flags |= CLONE_NEWIPC;
#endif
        return flags;
    }
#endif
}

int namespace_configure_child(const nexus_namespace_config_t *cfg)
{
    if (cfg == NULL)
        return NEXUS_OK;

#ifdef _WIN32
    /* Explicit: Windows has no namespaces. Requesting any isolation is an
     * unsupported configuration, never a silent success. */
    if (cfg->isolate_pid || cfg->isolate_mount || cfg->isolate_net ||
        cfg->isolate_uts || cfg->isolate_ipc)
        return NEXUS_ERR_UNSUPPORTED;
    return NEXUS_OK;
#else
    if (cfg->isolate_uts && cfg->hostname != NULL && cfg->hostname[0] != '\0') {
        if (sethostname(cfg->hostname, strlen(cfg->hostname)) != 0)
            return NEXUS_ERR_PROCESS; /* errno preserved for the caller */
    }

    if (cfg->isolate_mount) {
        /* Prevent mount-event propagation to the host. */
        if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
            return NEXUS_ERR_PROCESS;
    }

    return NEXUS_OK;
#endif
}

int namespace_apply(const nexus_namespace_config_t *cfg)
{
    if (cfg == NULL)
        return NEXUS_ERR_INVALID;

#ifndef _WIN32
    {
        int flags = namespace_clone_flags(cfg);
        if (flags != 0 && unshare(flags) != 0)
            return NEXUS_ERR_PROCESS; /* e.g. EPERM for an unprivileged user */
    }
#endif

    return namespace_configure_child(cfg);
}
