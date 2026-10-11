/*
 * NexusDB — Container Engine: Linux Namespace Isolation implementation.
 *
 * Real target: Ubuntu 22.04 / WSL2 using unshare() / clone().
 * Dev target: Windows fallback shim.
 */
#include "container/namespace.h"
#include "container/container.h"

#include <string.h>

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

int namespace_apply(const nexus_namespace_config_t *cfg)
{
    if (!cfg)
        return NEXUS_ERR_INVALID;

#ifndef _WIN32
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

    if (flags != 0) {
        if (unshare(flags) != 0) {
            /* If unshare fails (e.g. unprivileged user), return process error */
            return NEXUS_ERR_PROCESS;
        }
    }

    if (cfg->isolate_uts && cfg->hostname) {
        (void)sethostname(cfg->hostname, strlen(cfg->hostname));
    }

    if (cfg->isolate_mount) {
        /* Prevent mount event propagation to host */
        (void)mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL);
    }
#else
    (void)cfg;
#endif

    return NEXUS_OK;
}
