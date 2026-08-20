#!/bin/bash
# Deploy ProcessBus to the qotom target (default host: blackbox)
# and optionally run scenarios. Default: push files only, no rebuild.
#
# Manual one-time setup of the target (SSH key, packages, GRUB cmdline,
# BIOS) is described in README.md. RT tuning is applied automatically
# after every reboot via setup_platform.sh on the target.

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
REPO_DIR="$SCRIPT_DIR/../.."
INSTALL_DIR="${INSTALL_DIR:-$REPO_DIR/install-atom}"
COMMON_DIR="$REPO_DIR/deploy/common"
RESULTS_DIR="$SCRIPT_DIR/results"
SCENARIOS_FILE="$SCRIPT_DIR/scenarios.conf"
PARSE_SCRIPT="$COMMON_DIR/result_parser.sh"
BUNDLE="$SCRIPT_DIR/pbus"
SENTINEL="/var/run/pbus_rt"

HOST="blackbox"
USER="jarvis"
REMOTE_DIR="/home/jarvis/pbus"

DO_REBUILD=0
DO_PUSH=1
RUN_ALL=0
SCENARIO=""

usage() {
    cat <<EOF
Usage: $0 [--rebuild] [--no-push] [--scenario <name> | --all] [--host <addr>]

Default behaviour: push files only (no rebuild, no scenarios).

  --rebuild          Rebuild via ci/build.sh --rebuild before pushing
  --no-push          Skip pushing files and RT tuning; run against what is
                     already deployed (keeps the on-target binary untouched)
  --scenario <name>  Run a single scenario from scenarios.conf
  --all              Run all scenarios from scenarios.conf
  --host <addr>      Override target host (default: $HOST)
  -h, --help         Show this help

The target is auto-tuned (setup_platform.sh) if the sentinel
$SENTINEL is missing — i.e. once per boot.
EOF
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --rebuild)  DO_REBUILD=1 ;;
        --no-push)  DO_PUSH=0 ;;
        --scenario) SCENARIO="$2"; shift ;;
        --all)      RUN_ALL=1 ;;
        --host)     HOST="$2"; shift ;;
        -h|--help)  usage; exit 0 ;;
        *)          echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

# -n keeps ssh from consuming the scenario loop's stdin (the while-read file).
SSH="ssh -n -o StrictHostKeyChecking=no $USER@$HOST"
SCP="scp -o StrictHostKeyChecking=no"

# --- 1. Optional rebuild ---
if [ "$DO_REBUILD" -eq 1 ]; then
    echo "=== Rebuilding ==="
    "$REPO_DIR/ci/build.sh" --rebuild
fi

# --- 2. Bundle and push ---
if [ "$DO_PUSH" -eq 1 ]; then
    echo "=== Pushing files to $USER@$HOST:$REMOTE_DIR ==="
    rm -rf "$BUNDLE"
    mkdir -p "$BUNDLE"
    cp -rp "$INSTALL_DIR/"* "$BUNDLE/"
    cp -p  "$COMMON_DIR/device_runner.sh" "$BUNDLE/"
    cp -p  "$COMMON_DIR/result_parser.sh" "$BUNDLE/"
    cp -p  "$SCRIPT_DIR/setup_platform.sh" "$BUNDLE/"
    cp -p  "$SCRIPT_DIR/run_"*.sh          "$BUNDLE/"
    cp -p  "$SCRIPT_DIR/scenarios.conf"    "$BUNDLE/"

    $SSH "mkdir -p $REMOTE_DIR"
    $SCP -rp "$BUNDLE/"* "$USER@$HOST:$REMOTE_DIR/"

    # --- 3. Ensure RT tuning applied (idempotent via sentinel) ---
    if $SSH "test -e $SENTINEL"; then
        echo "=== Tuning already applied ($SENTINEL present) — skipping ==="
    else
        echo "=== Applying RT tuning + DPDK env ==="
        $SSH "cd $REMOTE_DIR && sudo bash setup_platform.sh"
    fi
else
    echo "=== Skipping push and RT tuning (--no-push) ==="
fi

# --- 4. Run scenarios (if requested) ---
if [ -z "$SCENARIO" ] && [ "$RUN_ALL" -eq 0 ]; then
    echo "=== Done (no scenario requested) ==="
    exit 0
fi

DATE_DIR=$(date +%Y_%m_%d__%H_%M)
OUT="$RESULTS_DIR/$DATE_DIR"
mkdir -p "$OUT"
TOTAL=0; PASSED=0; FAILED=0

START_DELAY=15                    # processor leads the generator
PROC_PAD=$((START_DELAY + 5))     # processor --time outlives the generator
DEV_OUT="/tmp/pbus_out"          # on-device capture dir
RUNNER="cd $REMOTE_DIR && sudo bash device_runner.sh"

# Launch device_runner (--action run) inside a detached tmux session on $HOST.
# NIC is omitted so the qotom role scripts use their hardcoded loopback NICs
# (0B gen / 0C proc). Args: role name app_args lcores duration time_pad
start_role() {
    local role="$1" nm="$2" a="$3" lc="$4" dur="$5" pad="$6" sess="pbus_$1"
    # Idempotent graceful clear of any prior run through the runner's own stop
    # path (SIGTERM the app, then close its session). Never an abrupt kill here.
    $SSH "$RUNNER --action stop --role $role" >/dev/null 2>&1 || true
    # tmux MUST run under the same user (root/sudo) as the wait/stop actions, or
    # they check a different tmux socket, see no session, and kill the app early.
    $SSH "cd $REMOTE_DIR && sudo tmux new-session -d -s $sess \
        'bash device_runner.sh --action run \
        --role $role --name \"$nm\" --args \"$a\" --lcores \"$lc\" \
        --duration $dur --time-pad $pad --outdir $DEV_OUT'"
    sleep 2
    $SSH "sudo tmux has-session -t $sess 2>/dev/null"
}

# Single-box: both roles run on $HOST. Lifecycle (tmux, SIGTERM, pre-flight) is
# owned by device_runner.sh on the target.
run_scenario() {
    local name="$1" gen_args="$2" proc_args="$3" gen_lcores="$4" proc_lcores="$5" duration="$6" max_lat="$7"
    TOTAL=$((TOTAL + 1))

    local mins=$((duration / 60)) secs=$((duration % 60))
    echo
    echo "==== [$TOTAL] $name  (${mins}m${secs}s) ============================"

    if ! $SSH "$RUNNER --action preflight"; then
        echo "==== [$TOTAL] $name: FAIL (preflight)"; FAILED=$((FAILED + 1)); return 0
    fi

    if ! start_role proc "$name" "$proc_args" "$proc_lcores" "$duration" "$PROC_PAD"; then
        echo "==== [$TOTAL] $name: FAIL (processor start)"
        $SSH "$RUNNER --action stop --role proc" || true
        FAILED=$((FAILED + 1)); return 0
    fi

    sleep "$START_DELAY"

    if ! start_role gen "$name" "$gen_args" "$gen_lcores" "$duration" 0; then
        echo "==== [$TOTAL] $name: FAIL (generator start)"
        $SSH "$RUNNER --action stop --role gen"  || true
        $SSH "$RUNNER --action stop --role proc" || true
        FAILED=$((FAILED + 1)); return 0
    fi

    $SSH "$RUNNER --action wait --role gen --duration '$duration'" || true
    $SSH "$RUNNER --action stop --role gen"  || true
    $SSH "$RUNNER --action stop --role proc" || true

    $SCP "$USER@$HOST:$DEV_OUT/${name}_gen"  "$OUT/${name}_gen"  2>/dev/null || true
    $SCP "$USER@$HOST:$DEV_OUT/${name}_proc" "$OUT/${name}_proc" 2>/dev/null || true

    if [ ! -s "$OUT/${name}_gen" ] || [ ! -s "$OUT/${name}_proc" ]; then
        echo "==== [$TOTAL] $name: FAIL (no logs collected)"; FAILED=$((FAILED + 1)); return 0
    fi

    local err
    err=$(grep -hm1 'Exception:\|EAL: Error\|No available ports' \
        "$OUT/${name}_gen" "$OUT/${name}_proc" 2>/dev/null || true)
    if [ -n "$err" ]; then
        FAILED=$((FAILED + 1)); echo "==== [$TOTAL] $name: FAIL  ($err)"; return 0
    fi

    bash "$PARSE_SCRIPT" "$OUT/${name}_gen" "$OUT/${name}_proc" \
        "$name" "$duration" "$max_lat" > "$OUT/${name}.summary.json"

    local pass max_us why
    pass=$(grep -o '"pass": [a-z]*'        "$OUT/${name}.summary.json" | awk '{print $2}')
    max_us=$(grep -o '"max_us": [0-9]*'    "$OUT/${name}.summary.json" | head -1 | awk '{print $2}')
    why=$(grep -o '"fail_reason": "[^"]*"' "$OUT/${name}.summary.json" | cut -d'"' -f4)

    if [ "$pass" = "true" ]; then
        PASSED=$((PASSED + 1)); echo "==== [$TOTAL] $name: PASS  (max ${max_us}us)"
    else
        FAILED=$((FAILED + 1)); echo "==== [$TOTAL] $name: FAIL  ($why)"
    fi
}

echo "=== Running scenarios on $HOST ==="
while IFS='|' read -r name gen proc gen_lcores proc_lcores dur maxlat; do
    [[ "$name" =~ ^[[:space:]]*# ]] && continue
    [[ -z "$name" ]] && continue
    name=$(echo        "$name"        | xargs)
    gen=$(echo         "$gen"         | xargs)
    proc=$(echo        "$proc"        | xargs)
    gen_lcores=$(echo  "$gen_lcores"  | xargs)
    proc_lcores=$(echo "$proc_lcores" | xargs)
    dur=$(echo         "$dur"         | xargs)
    maxlat=$(echo      "$maxlat"      | xargs)
    [ -n "$SCENARIO" ] && [ "$name" != "$SCENARIO" ] && continue
    run_scenario "$name" "$gen" "$proc" "$gen_lcores" "$proc_lcores" "$dur" "$maxlat"
done < "$SCENARIOS_FILE"

echo
echo "Results in: $OUT/"
echo "$PASSED passed, $FAILED failed (of $TOTAL)"
exit "$FAILED"
