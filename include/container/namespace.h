/*
 * NexusDB — Container Engine: Linux Namespace Isolation (Ayush: Systems Engineer)
 *
 * Implements process boundary isolation using Linux namespaces:
 *   - PID   (CLONE_NEWPID): isolated process tree; the workload becomes PID 1
 *   - MOUNT (CLONE_NEWNS):  private mount namespace (propagation made private)
 *   - NET   (CLONE_NEWNET): isolated network stack (loopback only)
 *   - UTS   (CLONE_NEWUTS): containerized hostname
 *   - IPC   (CLONE_NEWIPC): isolated SysV IPC / POSIX message queues
 *
 * PRIVILEGE MODEL (important — no privilege escalation is performed here):
 *   Creating PID/NET/UTS/IPC namespaces requires CAP_SYS_ADMIN in the current
 *   user namespace. An unprivileged process can only obtain that capability by
 *   first entering a user namespace it may legitimately create (e.g. the
 *   caller runs under `unshare -U --map-root-user` or a privileged service).
 *   This module does NOT create user namespaces and performs NO UID/GID
 *   mapping; it never elevates privileges. When the capabilities are absent
 *   the namespace syscalls fail and the caller must treat that as a hard
 *   failure (never a silent, unisolated start).
 *
 * IMPLICATIONS of the namespaces actually created:
 *   - MOUNT: a private mount namespace is created and propagation is made
 *     private; no new root is set up (no pivot_root/chroot), so the workload
 *     sees the host filesystem view unless it mounts over it.
 *   - NET:   the workload gets a fresh network stack whose only interface is a
 *     DOWN loopback. No veth/bridge/NAT is configured, so it has no external
 *     connectivity.
 *   - UTS:   hostname is set to cfg->hostname inside the namespace.
 */
#ifndef NEXUS_NAMESPACE_H
#define NEXUS_NAMESPACE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nexus_namespace_config {
    int isolate_pid;
    int isolate_mount;
    int isolate_net;
    int isolate_uts;
    int isolate_ipc;
    const char *hostname;
} nexus_namespace_config_t;

/* Initialize default namespace configuration */
void namespace_config_default(nexus_namespace_config_t *cfg, const char *container_name);

/* Apply namespace isolation in current process context (calls unshare(2)).
 * NOTE: unshare(CLONE_NEWPID) only affects *future* children of the caller, so
 * the caller is not itself placed in the new PID namespace. Use
 * namespace_clone_flags() + clone(2) (as the container lifecycle does) when the
 * workload itself must be PID 1. */
int namespace_apply(const nexus_namespace_config_t *cfg);

/* Translate cfg into the CLONE_NEW* mask for clone(2)/unshare(2).
 * Returns 0 for a NULL cfg or a cfg that requests no isolation (Windows always
 * returns 0 — namespaces are unavailable there). */
int namespace_clone_flags(const nexus_namespace_config_t *cfg);

/* Configure the calling process AFTER clone(2)/unshare(2) has placed it in the
 * new namespaces: set the UTS hostname and make mount propagation private.
 * Must run in the child, before exec. Returns NEXUS_OK, or NEXUS_ERR_PROCESS
 * with errno set when a requested namespace could not be configured. On Windows
 * returns NEXUS_ERR_UNSUPPORTED when any isolation is requested (explicit "not
 * supported", never a false success). */
int namespace_configure_child(const nexus_namespace_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_NAMESPACE_H */
