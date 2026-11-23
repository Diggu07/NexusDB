PS1="${NEXUSDB_CONTAINER}:\${PWD}>> "

if [[ -f /tmp/.nexusdb_dashboard.bash ]]; then
  . /tmp/.nexusdb_dashboard.bash
  PROMPT_COMMAND="__nexus_dashboard_render \"\$NEXUSDB_CONTAINER\"${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
fi

nexusdb() { /tmp/nexusdb "$@"; }

nxhelp() {
  cat <<'EOF'
Inside this container shell the active container is $NEXUSDB_CONTAINER.
Shortcuts (no need to repeat `nexusdb <container>`):

  status                  show this container
  list                    list all containers
  exec [command ...]      send a control request (e.g. `exec ping`)
  run [command ...]       alias for exec
  init                    initialize this database
  db <op> [args...]       db init|... for this container
  table <op> [args...]    table create|list for this container
  insert <table> <hex>    insert a record
  get <table> <page> <slot>
  update <table> <page> <slot> <hex>
  delete <table> <page> <slot>
  scan <table>            scan all records

Original CLI is still available as `nexusdb ...`.
EOF
}

status() { /tmp/nexusdb status "$NEXUSDB_CONTAINER" "$@"; }
list() { /tmp/nexusdb list "$@"; }
exec() { /tmp/nexusdb exec "$NEXUSDB_CONTAINER" "$@"; }
run() { exec "$@"; }

db() {
  if (( $# == 0 )); then
    /tmp/nexusdb db
    return $?
  fi
  /tmp/nexusdb db "$1" "$NEXUSDB_CONTAINER" "${@:2}"
}

table() {
  if (( $# == 0 )); then
    /tmp/nexusdb table
    return $?
  fi
  /tmp/nexusdb table "$1" "$NEXUSDB_CONTAINER" "${@:2}"
}

init() { /tmp/nexusdb db init "$NEXUSDB_CONTAINER" "$@"; }
insert() { /tmp/nexusdb insert "$NEXUSDB_CONTAINER" "$@"; }
get() { /tmp/nexusdb get "$NEXUSDB_CONTAINER" "$@"; }
update() { /tmp/nexusdb update "$NEXUSDB_CONTAINER" "$@"; }
delete() { /tmp/nexusdb delete "$NEXUSDB_CONTAINER" "$@"; }
scan() { /tmp/nexusdb scan "$NEXUSDB_CONTAINER" "$@"; }
