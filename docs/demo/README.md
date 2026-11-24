# End-to-end demo reproducibility

The committed transcript is `docs/demo/END_TO_END_DEMO.txt`. It was regenerated
on 2026-11-24 against the final Phase 2 code with `build-wsl`.

## WSL2 / Linux with delegated cgroups

Run as a user that can write `NEXUS_CGROUP_ROOT` and whose prompt ends in `$`.
The root prompt breaks the shell-integration prompt assertions.

```bash
mkdir -p /sys/fs/cgroup/nexusdb/devshell
echo $$ | tee /sys/fs/cgroup/nexusdb/devshell/cgroup.procs >/dev/null
export NEXUS_CGROUP_ROOT=/sys/fs/cgroup/nexusdb
bash scripts/demo_end_to_end.sh build-wsl
```

To reproduce the exact transcript from Windows without changing login shells:

```powershell
wsl -u root -e bash -lc 'cd /mnt/d/NexusDB && mkdir -p /sys/fs/cgroup/nexusdb/devshell && echo $$ | tee /sys/fs/cgroup/nexusdb/devshell/cgroup.procs >/dev/null && setpriv --reuid=1000 --regid=1000 --clear-groups --inh-caps=-all env HOME=/home/digvijay USER=digvijay LOGNAME=digvijay NEXUS_CGROUP_ROOT=/sys/fs/cgroup/nexusdb bash -lc "cd /mnt/d/NexusDB && bash scripts/demo_end_to_end.sh build-wsl"'
```

The transcript ends with `=== END-TO-END DEMO PASSED ===`.

`NEXUS_CGROUP_ROOT` must be an ancestor of the shell's current cgroup. Pointing
it at a sibling subtree makes `nexusdb start` attempt `systemd-run --user`, which
fails in a non-session `setpriv` shell with `Operation not permitted`.
