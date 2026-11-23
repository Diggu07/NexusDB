declare -A __NEXUS_DASH_PREV_USAGE=()
declare -A __NEXUS_DASH_PREV_TIME=()
__NEXUS_DASH_WIDTH=48
__NEXUS_DASH_BORDER=$(printf '%*s' $((__NEXUS_DASH_WIDTH - 2)) '' | tr ' ' '-')

__nexus_dh_human_bytes() {
  awk -v b="$1" 'BEGIN {
    if (b >= 1073741824) printf "%.1f GiB", b / 1073741824;
    else if (b >= 1048576) printf "%.1f MiB", b / 1048576;
    else if (b >= 1024) printf "%.1f KiB", b / 1024;
    else printf "%d B", b;
  }'
}

__nexus_dh_cgroup_dir() {
  local name=$1 root d
  if [[ -n ${NEXUSDB_CGROUP_PATH:-} ]]; then
    printf '%s\n' "$NEXUSDB_CGROUP_PATH"
    return 0
  fi
  for root in ${NEXUS_CGROUP_ROOT:-/sys/fs/cgroup} /sys/fs/cgroup; do
    for d in "$root/nexusdb/$name" "$root/$name"; do
      if [[ -d $d ]]; then
        printf '%s\n' "$d"
        return 0
      fi
    done
  done
  return 1
}

__nexus_dh_line() {
  printf '| %-*s |\n' $((__NEXUS_DASH_WIDTH - 4)) "$1"
}

__nexus_dh_panel() {
  local name=$1 dir=$2
  local mem_max mem_cur mem_left mem_pct cpu_quota cpu_period cpu_limit
  local cpu_usage cpu_used="n/a" pids state key now delta_usage delta_time
  local header="Memory: unavailable"

  [[ -r $dir/memory.max ]] && IFS= read -r mem_max < "$dir/memory.max"
  [[ -r $dir/memory.current ]] && IFS= read -r mem_cur < "$dir/memory.current"
  [[ -r $dir/cpu.max ]] && read -r cpu_quota cpu_period < "$dir/cpu.max"
  [[ -r $dir/cpu.stat ]] && cpu_usage=$(awk '/^usage_usec /{print $2; exit}' "$dir/cpu.stat" 2>/dev/null)
  [[ -r $dir/cgroup.procs ]] && pids=$(wc -l < "$dir/cgroup.procs" 2>/dev/null | tr -d ' ')

  if [[ ${mem_max:-} =~ ^[0-9]+$ && ${mem_cur:-} =~ ^[0-9]+$ && $mem_max -gt 0 ]]; then
    mem_left=$((mem_max - mem_cur))
    mem_pct=$((mem_cur * 100 / mem_max))
    header="Memory: $(__nexus_dh_human_bytes "$mem_cur") / $(__nexus_dh_human_bytes "$mem_max") ($mem_pct%)"
    mem_left="Free:   $(__nexus_dh_human_bytes "$mem_left")"
  elif [[ ${mem_cur:-} =~ ^[0-9]+$ ]]; then
    header="Memory: $(__nexus_dh_human_bytes "$mem_cur") / unlimited"
    mem_left="Free:   unlimited"
  else
    mem_left="Free:   unavailable"
  fi

  if [[ ${cpu_quota:-} == max ]]; then
    cpu_limit="unlimited"
  elif [[ ${cpu_quota:-} =~ ^[0-9]+$ && ${cpu_period:-} =~ ^[0-9]+$ && $cpu_period -gt 0 ]]; then
    cpu_limit="$((cpu_quota * 100 / cpu_period))%"
  else
    cpu_limit="unavailable"
  fi

  key=${name//-/_}
  now=$(date +%s%N 2>/dev/null || printf '0')
  if [[ ${cpu_usage:-} =~ ^[0-9]+$ ]]; then
    if [[ ${__NEXUS_DASH_PREV_USAGE[$key]:-} =~ ^[0-9]+$ &&
          ${__NEXUS_DASH_PREV_TIME[$key]:-} =~ ^[0-9]+$ &&
          $now -gt ${__NEXUS_DASH_PREV_TIME[$key]} &&
          $cpu_usage -ge ${__NEXUS_DASH_PREV_USAGE[$key]} ]]; then
      delta_usage=$((cpu_usage - __NEXUS_DASH_PREV_USAGE[$key]))
      delta_time=$((now - __NEXUS_DASH_PREV_TIME[$key]))
      cpu_used=$(awk -v du="$delta_usage" -v dt="$delta_time" 'BEGIN { printf "%.1f%%", du * 100000 / dt }')
    fi
    __NEXUS_DASH_PREV_USAGE[$key]=$cpu_usage
    __NEXUS_DASH_PREV_TIME[$key]=$now
  fi

  if [[ ${pids:-0} =~ ^[0-9]+$ && $pids -gt 0 ]]; then
    state=RUNNING
  else
    state=STOPPED
  fi

  printf '+%s+\n' "$__NEXUS_DASH_BORDER"
  printf '+-- %-*s+\n' $((__NEXUS_DASH_WIDTH - 5)) "${name} dashboard"
  __nexus_dh_line "$header"
  __nexus_dh_line "$mem_left"
  __nexus_dh_line "CPU:    limit $cpu_limit, used $cpu_used"
  __nexus_dh_line "Procs:  ${pids:-0}    State: $state"
  printf '+%s+\n' "$__NEXUS_DASH_BORDER"
}

__nexus_dashboard_render() {
  [[ ${NEXUSDB_DASHBOARD:-1} != 0 ]] || return 0
  [[ -t 1 ]] || return 0
  local name=${1:-} cols=${COLUMNS:-0} dir panel line start
  [[ -n $name ]] || return 0
  if (( cols == 0 )); then
    cols=$(tput cols 2>/dev/null || printf '0')
  fi
  (( cols >= __NEXUS_DASH_WIDTH + 20 )) || return 0
  dir=$(__nexus_dh_cgroup_dir "$name") || return 0
  panel=$(__nexus_dh_panel "$name" "$dir") || return 0
  start=$((cols - __NEXUS_DASH_WIDTH - 1))
  printf '\033[s'
  local row=1
  while IFS= read -r line; do
    printf '\033[%s;%sH%s\033[K' "$row" "$start" "$line"
    row=$((row + 1))
  done <<< "$panel"
  printf '\033[u'
}
