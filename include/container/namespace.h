/*
 * NexusDB — Container Engine: Linux Namespace Isolation (Ayush: Systems Engineer)
 *
 * Implements process boundary isolation using Linux namespaces:
 *   - PID: isolated process tree
 *   - MOUNT (CLONE_NEWNS): private filesystem views
 *   - NETWORK (CLONE_NEWNET): isolated loopback and network stack
 *   - UTS (CLONE_NEWUTS): containerized hostname
 *   - IPC (CLONE_NEWIPC): isolated IPC message queues and shared memory
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

/* Apply namespace isolation in current process context */
int namespace_apply(const nexus_namespace_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* NEXUS_NAMESPACE_H */
