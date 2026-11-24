#!/usr/bin/env bash
# NexusDB — Bash prompt integration (container-aware prompt).
#
# Sourcing this file defines a `nexus` shell wrapper plus a PROMPT_COMMAND
# hook that rebuilds $PS1 on every render:
#
#   no active context:  digvijay@MSI:/mnt/d/NexusDB$
#   entered context:    digvijay@MSI:/mnt/d/NexusDB$:testdb>>
#   isolated shell:     testdb:/container>>  (after `nexus shell <name>`)
#   after `nexus leave` digvijay@MSI:/mnt/d/NexusDB$
#
# Shell commands:
#   nexus <args...>      recommended interface: forwards everything unchanged
#                        to the nexusdb CLI (argv, stdout/stderr, exit codes)
#   nexus help|-h|--help unified reference (CLI help + shell-context section)
#   nexus enter <name>   by default launches the isolated shell for an existing
#                        container; `--context` forces the lightweight,
#                        non-isolated prompt-only behavior.
#   nexus shell <name>   launch an isolated shell (bwrap): only the container
#                        data dir is visible/writable as /container; `nexusdb`
#                        and the live engine socket are available inside
#   nexus start <name>   start + drop straight into the isolated shell;
#                        `--no-shell` keeps it context-only. Set
#                        NEXUSDB_ENTER_AUTO_SHELL=0 to disable globally.
#   nexus leave          clear the context (original PS1 is restored)
#   nexus exit           alias for `nexus leave`
#   nexus context [--verify] print the active container; clear it if stale
#
# The active context lives in $NEXUSDB_CONTEXT plus the __nexus_* locals; it
# is per-interactive-shell and needs no background process. A standalone
# C/C++ executable cannot mutate its parent shell, so the C/C++ CLI is never
# asked to: bash functions are the only supported mechanism here.
#
# Limitations (documented in the README):
#   - The prompt suffix only *selects* a container for later CLI commands;
#     context-only provides no isolation and does not enter namespaces.
#     Use `nexus shell <name>` for a real isolated filesystem view.
#   - Other PROMPT_COMMAND hooks that rewrite $PS1 wholesale can fight this
#     integration; source it last so the saved base prompt is final.

# Original prompt, captured once at source time. The hook always rebuilds PS1
# from this base, so switching or leaving contexts can never stack suffixes,
# and dynamic escapes in the base (e.g. \w, \u, \h) keep working normally.
NEXUSDB_PROMPT_BASE="${NEXUSDB_PROMPT_BASE:-${PS1:-}}"

# Resolve the CLI by the documented resolution order ($NEXUSDB_CLI first,
# then PATH, then the known build directories relative to this integration
# script). Never hardcoded to a repository path, and no silent fallback to
# some other binary: only an executable named `nexusdb` is ever used.
__nexus_script_dir() {
  local src=${BASH_SOURCE[0]:-"$0"}
  if [[ -f $src ]]; then
    printf '%s\n' "${src%/*}"
    return 0
  fi
  printf '%s\n' "$PWD"
}

if [[ -f $(__nexus_script_dir)/nexusdb-dashboard.bash ]]; then
  # shellcheck source=nexusdb-dashboard.bash
  . "$(__nexus_script_dir)/nexusdb-dashboard.bash"
fi

__nexus_cli() {
  if [[ -n ${NEXUSDB_CLI:-} && -x ${NEXUSDB_CLI} ]]; then
    printf '%s\n' "$NEXUSDB_CLI"
    return 0
  fi
  local found
  found=$(command -v nexusdb 2>/dev/null || true)
  if [[ -n $found && -x $found ]]; then
    printf '%s\n' "$found"
    return 0
  fi
  local dir build_dir candidate
  dir=$(__nexus_script_dir)
  for build_dir in build-wsl build-linux-clean build-linux build; do
    candidate="$dir/../$build_dir/nexusdb"
    if [[ -x $candidate ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

# Clear failure mode when the CLI cannot be located (never a silent fallback).
__nexus_cli_missing() {
  printf '%s\n' \
    'nexus: cannot find the nexusdb CLI. Fix with one of:' \
    '  export NEXUSDB_CLI=/abs/path/to/nexusdb          # direct path' \
    '  export PATH="<repo>/build-linux-clean:$PATH"     # directory on PATH' \
    '  cmake -S <repo> -B <repo>/build-linux-clean &&   # or build it:' \
    '    cmake --build <repo>/build-linux-clean --target nexusdb_cli'
}

# Mirror Manager::IsValidName: [A-Za-z0-9_-], non-empty, shorter than
# NEXUS_NAME_MAX (64). This is a prompt-safety gate (no newline/escape/$ can
# reach PS1); the CLI stays the authority on whether the container exists.
__nexus_name_valid() {
  local name=${1:-}
  [[ -n $name && ${#name} -lt 64 && $name =~ ^[A-Za-z0-9_-]+$ ]]
}

# Validate with the CLI's own data-root resolution: honor $NEXUSDB_DATA_ROOT
# when it is exported, otherwise validate from the project root the CLI lives
# in (the documented default data root ./nexusdb_data sits next to it). On
# anything else fall back to validating in the current directory. No eval and
# no hardcoded paths.
__nexus_validate_container() {
  local cli=$1 name=$2 status
  if [[ -n ${NEXUSDB_DATA_ROOT:-} ]]; then
    "$cli" status "$name" >/dev/null 2>&1
    return $?
  fi
  local root=${cli%/*}/..
  if ( cd "$root" 2>/dev/null ); then
    ( cd "$root" 2>/dev/null && "$cli" status "$name" >/dev/null 2>&1 )
    status=$?
    if (( status != 127 )); then
      return "$status"
    fi
  fi
  "$cli" status "$name" >/dev/null 2>&1
}

# Re-render PS1 from the saved base (+ the context suffix when one is active).
# Trailing spaces of the base are dropped while a suffix is shown, so the
# rendered prompt ends in `...$:testdb>>` as promised. Suffixes can never
# accumulate because the base is never mutated.
__nexus_context_verify() {
  local name=${NEXUSDB_CONTEXT:-} cli status
  [[ -n $name ]] || return 0
  cli=$(__nexus_cli) || return 0
  __nexus_validate_container "$cli" "$name"
  status=$?
  if (( status != 0 )); then
    unset NEXUSDB_CONTEXT
    __nexus_refresh_prompt
    printf 'nexus: cleared stale container context: %s\n' "$name" >&2
    return 1
  fi
  return 0
}

__nexus_prompt_hook() {
  if [[ -z ${NEXUSDB_PROMPT_BASE:-} ]]; then
    return 0
  fi
  local base=$NEXUSDB_PROMPT_BASE trimmed
  trimmed=${base%"${base##*[![:space:]]}"}
  if [[ -n ${NEXUSDB_CONTEXT:-} ]]; then
    PS1="$trimmed:${NEXUSDB_CONTEXT}>>"
    __nexus_dashboard_render "$NEXUSDB_CONTEXT"
  else
    PS1=$base
  fi
}

__nexus_refresh_prompt() {
  __nexus_prompt_hook
}

__nexus_auto_shell_enabled() {
  case ${NEXUSDB_ENTER_AUTO_SHELL:-1} in
  0 | false | FALSE | no | NO | off | OFF) return 1 ;;
  *) return 0 ;;
  esac
}

__nexus_cgroup_scope_ok() {
  [[ -r /proc/self/cgroup ]] || return 0
  [[ -d /sys/fs/cgroup/nexusdb ]] || return 0
  local cg
  IFS= read -r cg < /proc/self/cgroup || cg=
  [[ $cg == 0::/nexusdb/* ]]
}

__nexus_ensure_cgroup_scope() {
  __nexus_cgroup_scope_ok && return 0
  local root=/sys/fs/cgroup/nexusdb devshell
  devshell=$root/devshell
  if command -v sudo >/dev/null 2>&1; then
    sudo -n mkdir -p "$devshell" >/dev/null 2>&1 || true
    echo $$ | sudo -n tee "$devshell/cgroup.procs" >/dev/null 2>&1 || true
  fi
  if ! __nexus_cgroup_scope_ok; then
    printf 'nexus: shell is outside the delegated cgroup; start may fail.\n' >&2
    printf '       run: mkdir -p %s && echo $$ | sudo tee %s/cgroup.procs >/dev/null\n' \
      "$devshell" "$devshell" >&2
  fi
  return 0
}

__nexus_enter() {
  local context_only=0
  if [[ ${1:-} == --context ]]; then
    context_only=1
    shift
  fi
  if (( $# < 1 )) || [[ $1 == -h || $1 == --help ]]; then
    printf 'usage: nexus enter [--context] <container> [-- <command> [args...]]\n' >&2
    return 2
  fi
  local name=$1
  shift
  if ! __nexus_name_valid "$name"; then
    printf 'nexus: invalid container name: %s\n' "$name" >&2
    printf '       (allowed: [A-Za-z0-9_-], shorter than 64 chars)\n' >&2
    return 3
  fi
  local cli status
  cli=$(__nexus_cli) || { __nexus_cli_missing >&2; return 127; }
  __nexus_validate_container "$cli" "$name"
  status=$?
  if (( status != 0 )); then
    if (( status == 4 )); then
      printf 'nexus: no such container: %s\n' "$name" >&2
    fi
    # On failure keep the current context and prompt untouched.
    return "$status"
  fi
  if (( context_only == 0 )) && __nexus_auto_shell_enabled; then
    __nexus_shell "$name" "$@"
    return $?
  fi
  if (( $# != 0 )); then
    printf 'nexus: extra arguments require NEXUSDB_ENTER_AUTO_SHELL=1\n' >&2
    return 2
  fi
  NEXUSDB_CONTEXT=$name
  __nexus_refresh_prompt
  printf 'Entered container context: %s\n' "$name"
}

__nexus_leave() {
  if (( $# != 0 )); then
    printf 'usage: nexus leave\n' >&2
    return 2
  fi
  if [[ -z ${NEXUSDB_CONTEXT:-} ]]; then
    printf 'No container context is selected.\n' >&2
    return 1
  fi
  local name=$NEXUSDB_CONTEXT
  unset NEXUSDB_CONTEXT
  __nexus_refresh_prompt
  printf 'Left container context: %s\n' "$name"
}

__nexus_context() {
  if [[ ${1:-} == --verify ]]; then
    shift
  fi
  if (( $# != 0 )); then
    printf 'usage: nexus context [--verify]\n' >&2
    return 2
  fi
  if [[ -n ${NEXUSDB_CONTEXT:-} ]]; then
    __nexus_context_verify || return $?
    printf 'Active container: %s\n' "$NEXUSDB_CONTEXT"
    return 0
  fi
  printf 'No container context is selected.\n'
  return 1
}

__nexus_shell() {
  if (( $# < 1 )) || [[ $1 == -h || $1 == --help ]]; then
    printf 'usage: nexus shell <container> [-- <command> [args...]]\n' >&2
    return 2
  fi
  local name=$1
  shift
  if [[ ${1:-} == -- ]]; then
    shift
  fi
  if ! __nexus_name_valid "$name"; then
    printf 'nexus: invalid container name: %s\n' "$name" >&2
    printf '       (allowed: [A-Za-z0-9_-], shorter than 64 chars)\n' >&2
    return 3
  fi
  local cli status data_root data_dir d rc rc_file dash_file cgroup_dir
  local had_context=0 old_context=${NEXUSDB_CONTEXT:-}
  [[ -n $old_context ]] && had_context=1
  cli=$(__nexus_cli) || { __nexus_cli_missing >&2; return 127; }
  __nexus_validate_container "$cli" "$name"
  status=$?
  if (( status != 0 )); then
    if (( status == 4 )); then
      printf 'nexus: no such container: %s\n' "$name" >&2
    fi
    return "$status"
  fi
  if [[ -n ${NEXUSDB_DATA_ROOT:-} ]]; then
    data_root=$NEXUSDB_DATA_ROOT
  else
    data_root=${cli%/*}/../nexusdb_data
  fi
  data_dir=$data_root/$name
  if [[ ! -d $data_dir ]]; then
    printf 'nexus: no data directory for %s: %s\n' "$name" "$data_dir" >&2
    printf '       run `nexus start %s` or `nexus db init %s` first.\n' \
      "$name" "$name" >&2
    return 4
  fi
  if ! command -v bwrap >/dev/null 2>&1; then
    printf 'nexus: bwrap (bubblewrap) is required for `nexus shell`\n' >&2
    return 127
  fi

  local sock_path sock_dir
  if [[ -r $data_dir/nexusdb.sock.path ]]; then
    IFS= read -r sock_path < "$data_dir/nexusdb.sock.path" || sock_path=
    if [[ $sock_path == /* ]]; then
      sock_dir=${sock_path%/*}
    fi
  fi

  rc_file=$(cd "$(__nexus_script_dir)" 2>/dev/null && pwd)/nexusdb-container.bashrc
  dash_file=$(cd "$(__nexus_script_dir)" 2>/dev/null && pwd)/nexusdb-dashboard.bash
  cgroup_dir=$(__nexus_dh_cgroup_dir "$name" 2>/dev/null || true)

  local -a bw=(bwrap)
  for d in /bin /usr /lib /lib64 /sbin /etc; do
    [[ -e $d ]] && bw+=(--ro-bind "$d" "$d")
  done
  bw+=(--proc /proc --dev /dev --tmpfs /tmp --dir /.nexusdb-data
        --bind "$data_dir" "/.nexusdb-data/$name"
        --ro-bind "$cli" /tmp/nexusdb)
  if [[ -f $rc_file ]]; then
    bw+=(--ro-bind "$rc_file" /tmp/.nexusdb_bashrc)
  fi
  if [[ -f $dash_file ]]; then
    bw+=(--ro-bind "$dash_file" /tmp/.nexusdb_dashboard.bash)
  fi
  if [[ -n $cgroup_dir && -d $cgroup_dir ]]; then
    bw+=(--ro-bind "$cgroup_dir" /.cgroup --setenv NEXUSDB_CGROUP_PATH /.cgroup)
  fi
  if [[ -d $data_root/.nexus ]]; then
    bw+=(--bind "$data_root/.nexus" /.nexusdb-data/.nexus)
  fi
  bw+=(--setenv PATH "/tmp:${PATH:-/usr/bin:/bin}")
  if [[ -n ${sock_dir:-} && -d $sock_dir ]]; then
    bw+=(--ro-bind "$sock_dir" "$sock_dir")
  fi
  bw+=(--bind "$data_dir" /container --chdir /container
       --unshare-all --die-with-parent
       --setenv PS1 "${name}:\${PWD}>> "
       --setenv HOME /container
       --setenv NEXUSDB_CONTAINER "$name"
       --setenv NEXUSDB_DATA_ROOT /.nexusdb-data
       --setenv TERM "${TERM:-xterm}")
  if (( $# > 0 )); then
    bw+=("$@")
  elif [[ -f $rc_file ]]; then
    bw+=(/bin/bash --noprofile --rcfile /tmp/.nexusdb_bashrc -i)
  else
    bw+=(/bin/bash --noprofile --norc -i)
  fi

  NEXUSDB_CONTEXT=$name
  __nexus_refresh_prompt
  "${bw[@]}"
  rc=$?
  if (( had_context == 1 )); then
    NEXUSDB_CONTEXT=$old_context
  else
    unset NEXUSDB_CONTEXT
  fi
  __nexus_refresh_prompt
  return "$rc"
}

# Unified help: forward the CLI's authoritative help text unchanged, then
# append the only commands that live outside the binary (the shell context).
# No second list of CLI commands is maintained here.
__nexus_help() {
  local cli
  cli=$(__nexus_cli) || { __nexus_cli_missing >&2; return 127; }
  "$cli" help || return $?
  cat <<'EOF'

--- Shell context (this Bash session) ---
  nexus shell <name>   launch an isolated shell (bwrap): container data is
                       /container (HOME too); the `nexusdb` CLI is on PATH
                       at /tmp/nexusdb and the live engine socket is available.
                       Shortcuts: status list exec run init db table insert get
                       update delete scan; `nxhelp` prints them all. A right-side
                       panel shows memory/CPU; NEXUSDB_DASHBOARD=0 disables it
  nexus start <name>   start the container and drop straight into its isolated
                       shell (`name:/container>>`); migrates this shell into
                       the delegated cgroup when sudo is cached. Use `--no-shell`
                       for context-only.
  nexus enter <name>   by default launches `nexus shell <name>` (isolated).
                       Use `--context` for the prompt-only context; context-only
                       does NOT enter namespaces or provide isolation.
  Set NEXUSDB_ENTER_AUTO_SHELL=0 to disable auto-isolation globally.
  nexus stop <name>    stop the container; clears the context if it was active
  nexus leave / exit   clear the context and restore the original prompt
  nexus context [--verify] print the active container; clears stale contexts

`nexus` is the recommended interface; every command above is forwarded to
the compiled `nexusdb` CLI unchanged, and `nexusdb` itself keeps working
for direct CLI access.
EOF
}

# Shell entry point. Context commands, the isolated shell, unified help, and
# the start/stop/delete context transitions are handled here; every other
# invocation is forwarded to the real CLI with argv, stdout/stderr, and exit
# code intact, so existing commands (create/status/list/db/table/exec/...)
# are untouched.
nexus() {
  local command=${1:-}
  case $command in
  enter)
    shift
    __nexus_enter "$@"
    ;;
  leave | exit)
    __nexus_leave "${@:2}"
    ;;
  context)
    __nexus_context "${@:2}"
    ;;
  shell)
    shift
    __nexus_shell "$@"
    ;;
  start)
    shift
    local cli rc name no_shell=0 arg
    local -a start_args=()
    for arg in "$@"; do
      case $arg in
      --no-shell | --context-only) no_shell=1 ;;
      *) start_args+=("$arg") ;;
      esac
    done
    name=${start_args[0]:-}
    cli=$(__nexus_cli) || { __nexus_cli_missing >&2; return 127; }
    __nexus_ensure_cgroup_scope
    "$cli" start "${start_args[@]}"
    rc=$?
    if (( rc == 0 )) && (( no_shell == 0 )) && __nexus_name_valid "$name"; then
      NEXUSDB_CONTEXT=$name
      __nexus_refresh_prompt
      if __nexus_auto_shell_enabled; then
        __nexus_shell "$name"
        return $?
      fi
    fi
    return "$rc"
    ;;
  stop | delete)
    shift
    local op=$command cli rc name=${1:-}
    cli=$(__nexus_cli) || { __nexus_cli_missing >&2; return 127; }
    "$cli" "$op" "$@"
    rc=$?
    if (( rc == 0 )) && [[ ${NEXUSDB_CONTEXT:-} == "$name" ]]; then
      unset NEXUSDB_CONTEXT
      __nexus_refresh_prompt
    fi
    return "$rc"
    ;;
  help | -h | --help)
    if (( $# == 1 )); then
      __nexus_help
    else
      local cli
      cli=$(__nexus_cli) || { __nexus_cli_missing >&2; return 127; }
      "$cli" "$@"
    fi
    ;;
  *)
    local cli
    cli=$(__nexus_cli) || { __nexus_cli_missing >&2; return 127; }
    if [[ -z $command ]]; then
      "$cli"
    else
      "$cli" "$@"
    fi
    ;;
  esac
}

# Full uninstall: restore the original prompt, remove the hook from
# PROMPT_COMMAND (array or string form), and undefine the state/functions.
nexus_unintegrate() {
  if [[ -n ${NEXUSDB_PROMPT_BASE:-} ]]; then
    PS1=$NEXUSDB_PROMPT_BASE
  fi
  if declare -p PROMPT_COMMAND 2>/dev/null | grep -q 'declare -a'; then
    local kept=() cmd
    for cmd in ${PROMPT_COMMAND[@]}; do
      [[ $cmd == __nexus_prompt_hook ]] || kept+=("$cmd")
    done
    PROMPT_COMMAND=(${kept[@]+"${kept[@]}"})
  elif [[ ${PROMPT_COMMAND:-} == *__nexus_prompt_hook* ]]; then
    PROMPT_COMMAND=${PROMPT_COMMAND//__nexus_prompt_hook;/}
    PROMPT_COMMAND=${PROMPT_COMMAND/#__nexus_prompt_hook;/}
    PROMPT_COMMAND=${PROMPT_COMMAND/#__nexus_prompt_hook/}
  fi
  unset NEXUSDB_PROMPT_BASE NEXUSDB_CONTEXT
  unset -f nexus nexus_unintegrate __nexus_prompt_hook __nexus_refresh_prompt \
    __nexus_enter __nexus_leave __nexus_context __nexus_context_verify \
    __nexus_shell \
    __nexus_auto_shell_enabled __nexus_cgroup_scope_ok __nexus_ensure_cgroup_scope \
    __nexus_name_valid __nexus_validate_container __nexus_cli \
    __nexus_script_dir __nexus_cli_missing __nexus_help \
    __nexus_dashboard_render __nexus_dh_cgroup_dir __nexus_dh_human_bytes \
    __nexus_dh_panel __nexus_dh_line 2>/dev/null
  unset -v __NEXUS_DASH_PREV_USAGE __NEXUS_DASH_PREV_TIME \
    __NEXUS_DASH_WIDTH __NEXUS_DASH_BORDER 2>/dev/null
}

# Install the hook once, ahead of any existing PROMPT_COMMAND entries, and
# only if another instance is not already installed (idempotent re-source).
_nexus_hook_installed=$([[ ${PROMPT_COMMAND[*]-} == *__nexus_prompt_hook* ]] && echo 1 || echo 0)
if (( _nexus_hook_installed == 0 )); then
  if declare -p PROMPT_COMMAND 2>/dev/null | grep -q 'declare -a'; then
    PROMPT_COMMAND=(__nexus_prompt_hook ${PROMPT_COMMAND[@]+"${PROMPT_COMMAND[@]}"})
  else
    PROMPT_COMMAND="__nexus_prompt_hook${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
  fi
fi
unset _nexus_hook_installed
