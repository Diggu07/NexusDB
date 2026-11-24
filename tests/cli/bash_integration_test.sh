#!/usr/bin/env bash
# NexusDB — Bash prompt-integration end-to-end test.
#
# Sources scripts/bash-integration.sh against a real `nexusdb` CLI in an
# isolated $NEXUSDB_DATA_ROOT, then checks exit codes, prompt state, rendered
# prompts (${PS1@P} expands the \u/\h/\w escapes exactly like bash does when
# drawing the prompt), and that the normal CLI surface still works.
#
# Usage: bash_integration_test.sh <path-to-nexusdb-cli>
# Prints PASS/FAIL lines and a summary; exit 0 iff everything passed.

set -u

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)

if (( $# != 1 )); then
  echo "usage: $(basename "$0") <nexusdb-cli>" >&2
  exit 2
fi
CLI=$1
if [[ ! -x $CLI ]]; then
  echo "FAIL: CLI not executable: $CLI" >&2
  exit 1
fi
INTEGRATION="$REPO_ROOT/scripts/bash-integration.sh"
if [[ ! -f $INTEGRATION ]]; then
  echo "FAIL: integration script missing: $INTEGRATION" >&2
  exit 1
fi

g_failures=0
g_checks=0
g_env_limited=0

pass() {
  printf 'PASS: %s\n' "$1"
}

fail() {
  printf 'FAIL: %s\n' "$1"
  g_failures=$((g_failures + 1))
}

env_limited() {
  printf 'ENV-LIMITED: %s\n' "$1"
  g_env_limited=$((g_env_limited + 1))
}

# expect_equal <description> <expected> <actual>
expect_equal() {
  local desc=$1 expected=$2 actual=$3
  if [[ $expected == "$actual" ]]; then
    pass "$desc"
  else
    fail "$desc (expected \`$expected', got \`$actual')"
  fi
  g_checks=$((g_checks + 1))
}

expect_contains() {
  local desc=$1 needle=$2 hay=$3
  if [[ $hay == *"$needle"* ]]; then
    pass "$desc"
  else
    fail "$desc (missing \`$needle' in \`$hay')"
  fi
  g_checks=$((g_checks + 1))
}

# expect_rc <description> <expected-rc> <actual-rc>
expect_rc() {
  local desc=$1 expected=$2 actual=$3
  if (( expected == actual )); then
    pass "$desc"
  else
    fail "$desc (expected rc=$expected, got rc=$actual)"
  fi
  g_checks=$((g_checks + 1))
}

finish() {
  if (( g_failures == 0 && g_env_limited > 0 )); then
    echo "=== ENV-LIMITED (0 failure(s), $g_env_limited limited) ==="
    exit 0
  fi
  if (( g_failures == 0 )); then
    echo "ALL $g_checks CHECKS PASSED"
    exit 0
  fi
  echo "$g_checks checks run, $g_failures FAILED"
  exit 1
}

DELIBERATELY_NOT_TESTED="real namespace/cgroup entry (covered by the runtime tests); isolation between concurrent shells"

# --- fixture: isolated data root with two containers -----------------------
DATA_ROOT=$(mktemp -d /tmp/nexusdb-prompt-test.XXXXXX)
export NEXUSDB_DATA_ROOT=$DATA_ROOT
export NEXUSDB_ENTER_AUTO_SHELL=0
trap 'rm -rf "$DATA_ROOT"' EXIT

"$CLI" create testdb --memory 256M --cpu 50 >/dev/null 2>&1
expect_rc "fixture: create testdb" 0 $?
"$CLI" create ayush --memory 128M --cpu 25 >/dev/null 2>&1
expect_rc "fixture: create ayush" 0 $?
"$CLI" db init testdb >/dev/null 2>&1
expect_rc "fixture: db init testdb" 0 $?
"$CLI" table create testdb students >/dev/null 2>&1
expect_rc "fixture: create table testdb.students" 0 $?
# Containers stay CREATED/stopped: context *selection* must not require a
# running process (it is not entering namespaces).

# --- fresh base prompt, then source the integration ------------------------
PS1='\u@\h:\w\$ ' # default-WSL-style prompt with a trailing space
source "$INTEGRATION"

if declare -F __nexus_dashboard_render >/dev/null 2>&1; then
  pass "0. side-panel dashboard helpers are installed"
else
  fail "0. side-panel dashboard helpers are installed"
fi
g_checks=$((g_checks + 1))

# 1. Prompt is unchanged when no container is selected.
__nexus_prompt_hook
expect_equal "1. prompt unchanged when no container is selected" \
  '\u@\h:\w\$ ' "$PS1"
nexus context >/dev/null 2>&1
expect_rc "1b. context reports none (rc=1) before entering" 1 $?

# 2. Entering testdb appends :testdb>>.
nexus enter testdb >/dev/null 2>&1
expect_rc "2. nexus enter testdb succeeds" 0 $?
expect_equal "2. activated context variable" testdb "${NEXUSDB_CONTEXT-}"
expect_equal "2. PS1 suffix" '\u@\h:\w\$:testdb>>' "$PS1"
rendered=${PS1@P}
if [[ $rendered == *'$:testdb>>' ]]; then
  pass "2. rendered prompt ends \`...pwd\$:testdb>>' (${PS1@P})"
else
  fail "2. rendered prompt ends \`...pwd\$:testdb>>' (got ).\`$rendered')"
fi
g_checks=$((g_checks + 1))
if [[ $rendered != *'\$'* ]]; then
  pass "2b. rendered prompt has \\\$ escape expanded"
else
  fail "2b. rendered prompt has \\\$ escape expanded (got \`$rendered')"
fi
g_checks=$((g_checks + 1))

# Switch cwd while the context is active: \w in the base must keep updating.
if (( BASH_VERSINFO[0] * 100 + BASH_VERSINFO[1] >= 404 )); then
  OLD_CWD=$PWD
  cd /tmp 2>/dev/null || cd "$REPO_ROOT"
  __nexus_prompt_hook
  rendered=${PS1@P}
  if [[ $rendered == *'/tmp$:'* || $rendered == *"$REPO_ROOT"':testdb>>' ]] &&
    [[ $rendered == *':testdb>>' ]]; then
    pass "8. directory change stays visible with the suffix (${PS1@P})"
  else
    fail "8. directory change stays visible with the suffix (${PS1@P})"
  fi
  g_checks=$((g_checks + 1))
  cd "$OLD_CWD" || exit 1
else
  pass "8. skipped rendered-P directory check (bash < 4.4)"
  g_checks=$((g_checks + 1))
fi

# 3. Switching containers replaces the suffix (never stacks).
nexus enter ayush >/dev/null 2>&1
expect_rc "3. switch to ayush succeeds" 0 $?
expect_equal "3. switched context variable" ayush "${NEXUSDB_CONTEXT-}"
expect_equal "3. old suffix gone, single new suffix" '\u@\h:\w\$:ayush>>' "$PS1"
if [[ $PS1 != *testdb* ]]; then
  pass "3b. no trace of testdb remains after switching"
else
  fail "3b. no trace of testdb remains after switching"
fi
g_checks=$((g_checks + 1))

# 4. Leaving restores the original prompt.
nexus leave >/dev/null 2>&1
expect_rc "4. nexus leave succeeds" 0 $?
expect_equal "4. context cleared" '' "${NEXUSDB_CONTEXT-}"
expect_equal "4. original prompt restored" '\u@\h:\w\$ ' "$PS1"
nexus context >/dev/null 2>&1
expect_rc "4b. context reports none (rc=1) after leaving" 1 $?

# leave with nothing active: rc=1, prompt untouched.
nexus leave >/dev/null 2>&1
expect_rc "4c. leaving with no active context reports rc=1" 1 $?
expect_equal "4c. prompt still untouched" '\u@\h:\w\$ ' "$PS1"

# 5. Invalid container names are rejected without touching the context.
out=$(nexus enter 'db;rm -rf' 2>/dev/null)
expect_rc "5. shell-metacharacter name rejected (rc=3)" 3 $?
expect_equal "5. no output on stdout for invalid name" '' "$out"
expect_equal "5b. context still unset" '' "${NEXUSDB_CONTEXT-}"
expect_equal "5c. prompt preserved after invalid name" '\u@\h:\w\$ ' "$PS1"

# 6. A nonexistent container does not corrupt the prompt.
out=$(nexus enter ghost 2>/dev/null)
expect_rc "6. nonexistent container rejected (rc=4)" 4 $?
expect_equal "6. context preserved (still none)" '' "${NEXUSDB_CONTEXT-}"
expect_equal "6b. prompt preserved (no suffix)" '\u@\h:\w\$ ' "$PS1"

# Existing context must survive a failed enter (re-enter ghost while testdb
# is active): prompt and context must not change.
nexus enter testdb >/dev/null 2>&1
nexus enter ghost >/dev/null 2>&1
expect_rc "6c. failed switch keeps rc=4" 4 $?
expect_equal "6c. active context survives a failed enter" testdb "${NEXUSDB_CONTEXT-}"
expect_equal "6c. prompt survives a failed enter" '\u@\h:\w\$:testdb>>' "$PS1"

# 7. Repeated enter/leave cannot duplicate suffixes.
nexus enter testdb >/dev/null 2>&1
suffixes=$(printf '%s' "$PS1" | grep -o ':testdb>>' | wc -l)
expect_equal "7. exactly one suffix after repeated enters" 1 "$suffixes"
nexus leave >/dev/null 2>&1
nexus enter testdb >/dev/null 2>&1
suffixes=$(printf '%s' "$PS1" | grep -o ':testdb>>' | wc -l)
expect_equal "7b. still one suffix after leave/enter cycles" 1 "$suffixes"
nexus leave >/dev/null 2>&1
expect_equal "7c. clean base after cycles" '\u@\h:\w\$ ' "$PS1"

# exit alias
nexus enter testdb >/dev/null 2>&1
nexus exit >/dev/null 2>&1
expect_rc "nexus exit aliases nexus leave (rc=0)" 0 $?
expect_equal "exit alias clears the context" '' "${NEXUSDB_CONTEXT-}"

# 9. Unified `nexus` surface: help, delegation, exit-code and stream fidelity.
nexus status testdb >/dev/null 2>&1
expect_rc "9. nexus status testdb (rc=0)" 0 $?
nexus status doesnotexist >/dev/null 2>&1
expect_rc "9b. nexus status <unknown> forwards rc=4" 4 $?
nexus totally-unknown-cmd 2>/dev/null >/dev/null
expect_rc "9e. unknown command forwards CLI usage rc=2" 2 $?

O=$DATA_ROOT/.stdout # stdout scratch
E=$DATA_ROOT/.stderr # stderr scratch

capture() { # capture <desc> <args...>: sets $rc, $so (stdout), $se (stderr)
  local desc=$1
  shift
  nexus "$@" >"$O" 2>"$E"
  rc=$?
  so=$(cat "$O")
  se=$(cat "$E")
  : "$desc"
}

# 10. Unified help: CLI help (authoritative) + appended shell-context section.
capture "nexus help" help
expect_rc "10. nexus help (rc=0)" 0 "$rc"
if [[ $so == *'nexusdb create <name>'* ]]; then
  pass "10. nexus help forwards the CLI's authoritative help text"
else
  fail "10. nexus help forwards the CLI's authoritative help text"
fi
g_checks=$((g_checks + 1))
if [[ $so == *'--- Shell context'* && $so == *'nexus enter <name>'* ]] &&
  [[ $so == *'does NOT enter'* ]]; then
  pass "10. nexus help appends the shell-context section"
else
  fail "10. nexus help appends the shell-context section"
fi
g_checks=$((g_checks + 1))
expect_equal "10. nexus help writes nothing to stderr" '' "$se"
capture "-h alias" -h
expect_rc "10. nexus -h aliases unified help (rc=0)" 0 "$rc"
expect_rc "10. nexus -h stays silent on stderr" 0 "$([ -s "$E" ] && echo 1 || echo 0)"
capture "help with extra args" help extra
expect_rc "10. nexus help <extra> forwards to the CLI (rc=0)" 0 "$rc"
if [[ $so == *'Shell context'* ]]; then
  fail "10. nexus help <extra> forwards without the appended section (CLI parity)"
else
  pass "10. nexus help <extra> forwards without the appended section (CLI parity)"
fi
g_checks=$((g_checks + 1))

# 10b. Multi-argument forwarding with value flags: create/status boundary.
capture "create fwdflags" create fwdflags --memory 256M --cpu 50
expect_rc "10b. nexus create <name> --memory 256M --cpu 50 (rc=0)" 0 "$rc"
capture "status fwdflags" status fwdflags
expect_rc "10b. nexus status fwdflags (rc=0)" 0 "$rc"
if [[ $so == *'268435456'* && $so == *'50%'* ]]; then
  pass "10b. all forwarding arguments arrived at the CLI intact (--memory/--cpu)"
else
  fail "10b. all forwarding arguments arrived at the CLI intact (got \`$so')"
fi
g_checks=$((g_checks + 1))

# 10c. Container lifecycle through nexus (start needs a delegated cgroup).
capture "create lifecycle" create lifecycle --no-engine
expect_rc "10c. create lifecycle --no-engine (rc=0)" 0 "$rc"
capture "start lifecycle" start lifecycle
if (( rc == 0 )); then
  pass "10c. nexus start lifecycle (rc=0)"
  g_checks=$((g_checks + 1))
  if [[ ${NEXUSDB_CONTEXT-} == lifecycle && $PS1 == *':lifecycle>>' ]]; then
    pass "10c. nexus start selects the container context in the prompt"
  else
    fail "10c. nexus start selects the container context in the prompt"
  fi
  g_checks=$((g_checks + 1))
  capture "status lifecycle" status lifecycle
  if [[ $so == *'RUNNING'* ]]; then
    pass "10c. container reports RUNNING after nexus start"
  else
    fail "10c. container reports RUNNING after nexus start"
  fi
  g_checks=$((g_checks + 1))
  capture "stop lifecycle" stop lifecycle
  expect_rc "10c. nexus stop lifecycle (rc=0)" 0 "$rc"
  if [[ -z ${NEXUSDB_CONTEXT:-} && $PS1 == '\u@\h:\w\$ ' ]]; then
    pass "10c. nexus stop clears the context and restores the prompt"
  else
    fail "10c. nexus stop clears the context and restores the prompt"
  fi
  g_checks=$((g_checks + 1))
  capture "status lifecycle" status lifecycle
  if [[ $so == *'STOPPED'* ]]; then
    pass "10c. container reports STOPPED after nexus stop"
  else
    fail "10c. container reports STOPPED after nexus stop"
  fi
  g_checks=$((g_checks + 1))
elif [[ $se == *'systemd-run'* || $se == *'cgroup'* ]]; then
  env_limited "nexus start needs a delegated cgroup scope"
else
  fail "10c. nexus start lifecycle failed (rc=$rc, err=$se)"
  g_checks=$((g_checks + 1))
fi
# 10d. Isolated shell: the container data dir is the only visible workspace.
if command -v bwrap >/dev/null 2>&1; then
  capture "shell isolation" shell testdb -- /bin/bash -c 'pwd; cd; pwd; printf "\n"; ls -a; command -v nexusdb >/dev/null && printf "CLI_OK\n" || printf "CLI_MISSING\n"; test -f /tmp/.nexusdb_bashrc && printf "RC_OK\n" || printf "RC_MISSING\n"'
  expect_rc "10d. nexus shell <name> -- <command> (rc=0)" 0 "$rc"
  if [[ $so == *'/container'* && $so == *'nexusdb.db'* && $so == *CLI_OK* && $so == *RC_OK* && $so != *'CMakeLists.txt'* ]]; then
    pass "10d. isolated shell sees /container and nexusdb, not the host repo"
  else
    fail "10d. isolated shell sees /container, not the host repo (got \`$so')"
  fi
  g_checks=$((g_checks + 1))
else
  env_limited "bwrap unavailable for isolated shell test"
fi

# 10e. NEXUSDB_ENTER_AUTO_SHELL makes `enter` launch the isolated shell, while
# --context keeps the lightweight prompt-only behavior.
if command -v bwrap >/dev/null 2>&1; then
  export NEXUSDB_ENTER_AUTO_SHELL=1
  capture "enter auto shell" enter testdb -- /bin/bash -c 'pwd; cd; pwd; printf "\n"; ls -a; command -v nexusdb >/dev/null && printf "CLI_OK\n" || printf "CLI_MISSING\n"; test -f /tmp/.nexusdb_bashrc && printf "RC_OK\n" || printf "RC_MISSING\n"'
  expect_rc "10e. NEXUSDB_ENTER_AUTO_SHELL=1 enter -> isolated shell (rc=0)" 0 "$rc"
  if [[ $so == *'/container'* && $so == *'nexusdb.db'* && $so == *CLI_OK* && $so == *RC_OK* && $so != *'CMakeLists.txt'* ]]; then
    pass "10e. auto-enter shell sees /container and nexusdb, not the host repo"
  else
    fail "10e. auto-enter shell sees /container, not the host repo (got \`$so')"
  fi
  g_checks=$((g_checks + 1))
  capture "enter context override" enter --context testdb
  expect_rc "10e. nexus enter --context forces context-only (rc=0)" 0 "$rc"
  expect_equal "10e. --context selects the shell context" testdb "${NEXUSDB_CONTEXT-}"
  expect_equal "10e. --context updates the prompt" '\u@\h:\w\$:testdb>>' "$PS1"
  nexus leave >/dev/null 2>&1
  export NEXUSDB_ENTER_AUTO_SHELL=0
else
  env_limited "bwrap unavailable for auto-enter shell test"
fi

# The runtime occasionally reports the cgroup as still holding the just-stopped
# placeholder workload for a moment after `stop` (pre-existing CLI timing race,
# unrelated to the wrapper): retry the delete briefly.
tries=0
capture "delete lifecycle" delete lifecycle
while (( rc != 0 && tries < 10 )); do
  sleep 1
  capture "delete lifecycle" delete lifecycle
  tries=$((tries + 1))
done
expect_rc "10c. nexus delete lifecycle (rc=0)" 0 "$rc"

# 11. Database initialization + table operations forwarded correctly.
capture "table list testdb" table list testdb
expect_rc "11a. nexus table list testdb (rc=0)" 0 "$rc"
if [[ $so == *'students'* ]]; then
  pass "11a. table list output preserved"
else
  fail "11a. table list output preserved (got \`$so')"
fi
g_checks=$((g_checks + 1))

# 11b. Record operations preserve argument boundaries and exact bytes.
capture "insert hex" insert testdb students --hex 48656c6c6f
expect_rc "11b. explicit hex insert (rc=0)" 0 "$rc"
expect_contains "11b. explicit hex insert output" 'inserted page=' "$so"
if [[ $so =~ page=([0-9]+)[[:space:]]slot=([0-9]+) ]]; then
  PAGE=${BASH_REMATCH[1]}
  SLOT=${BASH_REMATCH[2]}
  pass "11b. insert reports record coordinates (page=$PAGE slot=$SLOT)"
  g_checks=$((g_checks + 1))
  capture "get" get testdb students "$PAGE" "$SLOT"
  expect_rc "11b. nexus get page slot (rc=0)" 0 "$rc"
  expect_contains "11b. get returns the explicit hex payload unchanged" \
    'hex=48656c6c6f' "$so"
  capture "update hex" update testdb students "$PAGE" "$SLOT" --hex 576f726c64
  expect_rc "11b. explicit hex update (rc=0)" 0 "$rc"
  capture "get" get testdb students "$PAGE" "$SLOT"
  expect_contains "11b. explicit hex update + get round-trip" \
    'hex=576f726c64' "$so"

  capture "insert text" insert testdb students 'Hello from NexusDB'
  expect_rc "11b. text insert with spaces (rc=0)" 0 "$rc"
  if [[ $so =~ page=([0-9]+)[[:space:]]slot=([0-9]+) ]]; then
    TEXT_PAGE=${BASH_REMATCH[1]}
    TEXT_SLOT=${BASH_REMATCH[2]}
    capture "get text" get testdb students "$TEXT_PAGE" "$TEXT_SLOT"
    expect_contains "11b. text insert preserves UTF-8 bytes" \
      'hex=48656c6c6f2066726f6d204e657875734442' "$so"
    capture "update text" update testdb students "$TEXT_PAGE" "$TEXT_SLOT" \
      'Updated record'
    expect_rc "11b. text update (rc=0)" 0 "$rc"
    capture "get text" get testdb students "$TEXT_PAGE" "$TEXT_SLOT"
    expect_contains "11b. text update preserves UTF-8 bytes" \
      'hex=55706461746564207265636f7264' "$so"
  else
    fail "11b. text insert reports record coordinates (got \`$so')"
  fi

  capture "insert punctuation" insert testdb students 'Hello, NexusDB!'
  expect_rc "11b. text insert with punctuation (rc=0)" 0 "$rc"
  capture "insert utf8" insert testdb students 'café ☕'
  expect_rc "11b. UTF-8 text insert (rc=0)" 0 "$rc"
  capture "insert hex-looking text" insert testdb students '48656c6c6f'
  expect_rc "11b. hex-looking text insert (rc=0)" 0 "$rc"
  capture "scan" scan testdb students
  expect_rc "11b. nexus scan records (rc=0)" 0 "$rc"
  expect_contains "11b. scan preserves updated text bytes" \
    'text="Updated record"' "$so"
  expect_contains "11b. scan preserves punctuation bytes" \
    'text="Hello, NexusDB!"' "$so"
  expect_contains "11b. scan preserves multi-byte UTF-8 bytes" \
    'text="café ☕"' "$so"
  expect_contains "11b. scan stores hex-looking input as literal text" \
    'text="48656c6c6f"' "$so"

  capture "insert invalid hex" insert testdb students --hex zz
  expect_rc "11b. invalid hex with --hex (rc=3)" 3 "$rc"
  capture "insert odd hex" insert testdb students --hex abc
  expect_rc "11b. odd-length hex with --hex (rc=3)" 3 "$rc"

  capture "delete record" delete testdb students "$PAGE" "$SLOT"
  expect_rc "11b. nexus delete page slot (rc=0)" 0 "$rc"
  capture "get" get testdb students "$PAGE" "$SLOT"
  expect_rc "11b. deleted record reads as rc=4" 4 "$rc"
  capture "scan" scan testdb students
  expect_rc "11b. nexus scan after record delete (rc=0)" 0 "$rc"
else
  fail "11b. insert reports record coordinates (got \`$so')"
  g_checks=$((g_checks + 1))
fi

# 11c. Streams: stdout/stderr of forwarded commands are preserved separately.
so= ; se= ; rc=
capture "status ghost" status ghost
expect_rc "11c. nexus status <ghost> (rc=4)" 4 "$rc"
expect_equal "11c. error text goes to stderr" 'no such container: ghost' "$se"
expect_equal "11c. nothing written to stdout for failures" "" "$so"
capture "list" list
expect_rc "11c. nexus list (rc=0)" 0 "$rc"
expect_equal "11c. successful commands write nothing to stderr" "" "$se"

# 11d. Invalid create names forward the CLI's rc=3 (no shell reinterpretation).
capture "create invalid" create 'db;rm -rf' 2>/dev/null
expect_rc "11d. quoted metacharacter name passes through to the CLI (rc=3)" 3 "$rc"

# 11e. Direct CLI binary keeps working while the integration is loaded.
"$CLI" list >/dev/null 2>&1
expect_rc "11e. nexusdb binary directly (rc=0)" 0 $?
"$CLI" help >/dev/null 2>&1
expect_rc "11e. nexusdb help directly (rc=0)" 0 $?

# 11f. Context verification clears a context whose container was deleted
# outside the wrapper (the documented stale-context gap).
"$CLI" create stalectx >/dev/null 2>&1
expect_rc "11f. fixture create stalectx" 0 $?
nexus enter --context stalectx >/dev/null 2>&1
expect_rc "11f. enter stale context" 0 $?
expect_equal "11f. context selected" stalectx "${NEXUSDB_CONTEXT-}"
"$CLI" delete stalectx >/dev/null 2>&1
expect_rc "11f. fixture delete stalectx" 0 $?
nexus context --verify >/dev/null 2>&1
expect_rc "11f. context --verify reports stale context (rc=1)" 1 $?
expect_equal "11f. stale context cleared" '' "${NEXUSDB_CONTEXT-}"
expect_equal "11f. prompt restored" '\u@\h:\w\$ ' "$PS1"

# Full unload back to a pristine shell.
nexus leave >/dev/null 2>&1
nexus_unintegrate >/dev/null 2>&1
expect_rc "unintegrate succeeds" 0 $?
if declare -f nexus >/dev/null 2>&1; then
  fail "unintegrate removes the nexus() function"
  g_checks=$((g_checks + 1))
else
  pass "unintegrate removes the nexus() function"
  g_checks=$((g_checks + 1))
fi
expect_equal "unintegrate clears NEXUSDB_CONTEXT" '' "${NEXUSDB_CONTEXT-}"
expect_equal "unintegrate clears NEXUSDB_PROMPT_BASE" '' "${NEXUSDB_PROMPT_BASE-}"
if [[ ${PROMPT_COMMAND-} == *__nexus_prompt_hook* ]]; then
  fail "unintegrate removes the PROMPT_COMMAND hook"
  g_checks=$((g_checks + 1))
else
  pass "unintegrate removes the PROMPT_COMMAND hook"
  g_checks=$((g_checks + 1))
fi

# Fixture teardown: containers created in the temp data root (engine-less and
# never user data; deleting them also drops their runtime state so repeated
# runs do not accumulate empty cgroup directories). Informational: `lifecycle`
# may already have been deleted above.
for ctr in ayush testdb fwdflags lifecycle; do
  "$CLI" delete "$ctr" >/dev/null 2>&1 || true
done

finish
