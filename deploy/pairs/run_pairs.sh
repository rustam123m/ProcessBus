#!/bin/bash
# Cross-device scenarios from atom_opi3b.conf: generator on one board, processor on
# the other. Deploy device_runner.sh + role scripts to both boards first; this
# script only triggers and collects.
#
# Lifecycle is owned by the on-device device_runner.sh (tmux, SIGTERM-first,
# pre-flight idle check). The host never backgrounds device processes and never
# kill -9s them; it calls device_runner actions in sequence and scp's the logs.

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
REPO_DIR="$SCRIPT_DIR/../.."
RESULTS_DIR="$SCRIPT_DIR/results"
PAIRS_FILE="$SCRIPT_DIR/atom_opi3b.conf"
PARSE_SCRIPT="$REPO_DIR/deploy/common/result_parser.sh"

START_DELAY=15          # processor leads the generator by this much
PROC_PAD=$((START_DELAY + 5))   # processor --time outlives the generator
DEV_OUT="/tmp/pbus_out"         # on-device capture dir
RUNNER="sudo bash device_runner.sh"

RUN_ALL=0
SCENARIO=""

usage() {
    cat <<EOF
Usage: $0 [--scenario <name> | --all] [--list]

  --scenario <name>  Run a single scenario from atom_opi3b.conf
  --all              Run every scenario in atom_opi3b.conf
  --list             Print scenario names and exit
  -h, --help         Show this help
EOF
}

function host_config()
{
    case "$1" in
        blackbox) HOST_ADDR="192.168.100.103"; HOST_DIR="/home/jarvis/pbus"; HOST_NIC="0000:07:00.0" ;;
        opi3b)    HOST_ADDR="192.168.100.105"; HOST_DIR="/home/jarvis/pbus"; HOST_NIC="0000:01:00.0" ;;
        *) echo "Unknown host: $1" >&2; exit 1 ;;
    esac
}

# ConnectTimeout bounds the connect; ServerAlive bounds a wedged session.
function remote()
{
    ssh -n -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "jarvis@$1" "$2"
}

function fetch()
{
    scp -q -o StrictHostKeyChecking=no -o ConnectTimeout=10 "jarvis@$1:$2" "$3"
}

# Launch device_runner (--action run) INSIDE a detached tmux session on the
# device. The app runs in the foreground of device_runner; tmux provides the
# detachment (no host-side '&'). Returns non-zero if the session did not come up.
# Args: addr dir role name app_args lcores nic duration time_pad
function start_role()
{
    local addr="$1" dir="$2" role="$3" nm="$4" a="$5" lc="$6" nic="$7" dur="$8" pad="$9"
    local sess="pbus_$role"
    # Idempotent graceful clear of any prior run through the runner's own stop
    # path (SIGTERM the app, then close its session). Never an abrupt kill here.
    remote "$addr" "cd $dir && $RUNNER --action stop --role $role" >/dev/null 2>&1 || true
    # tmux MUST run under the same user (root/sudo) as the wait/stop actions, or
    # they check a different tmux socket, see no session, and kill the app early.
    remote "$addr" "cd $dir && sudo tmux new-session -d -s $sess \
        'bash device_runner.sh --action run \
        --role $role --name \"$nm\" --args \"$a\" --lcores \"$lc\" --nic \"$nic\" \
        --duration $dur --time-pad $pad --outdir $DEV_OUT'"
    sleep 2
    remote "$addr" "sudo tmux has-session -t $sess 2>/dev/null"
}

# One folder per batch. A caller running many single --scenario invocations can
# export SESSION_DIR once so they all collect into the same dated folder instead
# of each stamping its own minute.
DATE_DIR="${SESSION_DIR:-$(date +%Y_%m_%d__%H_%M)}"
OUT="$RESULTS_DIR/$DATE_DIR"
TOTAL=0; PASSED=0; FAILED=0

run_pair() {
    local name="$1" gen_host="$2" gen_args="$3" proc_host="$4" proc_args="$5"
    local gen_lcores="$6" proc_lcores="$7" duration="$8" max_lat="$9"
    TOTAL=$((TOTAL + 1))

    host_config "$gen_host";  local gen_addr="$HOST_ADDR"  gen_dir="$HOST_DIR"  gen_nic="$HOST_NIC"
    host_config "$proc_host"; local proc_addr="$HOST_ADDR" proc_dir="$HOST_DIR" proc_nic="$HOST_NIC"

    echo
    echo "==== [$TOTAL] $name  ($gen_host -> $proc_host, ${duration}s) ===="

    # Rule 5: pre-flight each distinct device (clean slate) BEFORE any start.
    if ! remote "$proc_addr" "cd $proc_dir && $RUNNER --action preflight"; then
        echo "==== [$TOTAL] $name: FAIL (proc host unreachable/preflight)"; FAILED=$((FAILED+1)); return 0
    fi
    if [ "$gen_addr" != "$proc_addr" ]; then
        if ! remote "$gen_addr" "cd $gen_dir && $RUNNER --action preflight"; then
            echo "==== [$TOTAL] $name: FAIL (gen host unreachable/preflight)"; FAILED=$((FAILED+1)); return 0
        fi
    fi

    # Processor first (device_runner run, inside tmux).
    if ! start_role "$proc_addr" "$proc_dir" proc "$name" "$proc_args" "$proc_lcores" "$proc_nic" "$duration" "$PROC_PAD"; then
        echo "==== [$TOTAL] $name: FAIL (processor start)"
        remote "$proc_addr" "cd $proc_dir && $RUNNER --action stop --role proc" || true
        FAILED=$((FAILED+1)); return 0
    fi

    sleep "$START_DELAY"   # EAL init + link up before traffic

    # Generator.
    if ! start_role "$gen_addr" "$gen_dir" gen "$name" "$gen_args" "$gen_lcores" "$gen_nic" "$duration" 0; then
        echo "==== [$TOTAL] $name: FAIL (generator start)"
        remote "$gen_addr"  "cd $gen_dir  && $RUNNER --action stop --role gen"  || true
        remote "$proc_addr" "cd $proc_dir && $RUNNER --action stop --role proc" || true
        FAILED=$((FAILED+1)); return 0
    fi

    # Wait for the generator to finish its own --time, then stop both gracefully.
    remote "$gen_addr"  "cd $gen_dir  && $RUNNER --action wait --role gen --duration '$duration'" || true
    remote "$gen_addr"  "cd $gen_dir  && $RUNNER --action stop --role gen"  || true
    remote "$proc_addr" "cd $proc_dir && $RUNNER --action stop --role proc" || true

    # Rule 4: collect the on-device captures into the dated folder.
    mkdir -p "$OUT"
    fetch "$gen_addr"  "$DEV_OUT/${name}_gen"  "$OUT/${name}_gen"  || true
    fetch "$proc_addr" "$DEV_OUT/${name}_proc" "$OUT/${name}_proc" || true

    if [ ! -s "$OUT/${name}_gen" ] || [ ! -s "$OUT/${name}_proc" ]; then
        echo "==== [$TOTAL] $name: FAIL (no logs collected)"; FAILED=$((FAILED+1)); return 0
    fi

    local err
    err=$(grep -hm1 'Exception:\|EAL: Error' "$OUT/${name}_gen" "$OUT/${name}_proc" || true)
    if [ -n "$err" ]; then
        echo "==== [$TOTAL] $name: FAIL ($err)"; FAILED=$((FAILED+1)); return 0
    fi

    bash "$PARSE_SCRIPT" "$OUT/${name}_gen" "$OUT/${name}_proc" \
        "$name" "$duration" "$max_lat" > "$OUT/${name}.summary.json"

    local pass why
    pass=$(grep -o '"pass": [a-z]*'        "$OUT/${name}.summary.json" | awk '{print $2}')
    why=$( grep -o '"fail_reason": "[^"]*"' "$OUT/${name}.summary.json" | cut -d'"' -f4)
    if [ "$pass" = "true" ]; then
        PASSED=$((PASSED + 1)); echo "==== [$TOTAL] $name: PASS"
    else
        FAILED=$((FAILED + 1)); echo "==== [$TOTAL] $name: FAIL ($why)"
    fi
}

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --scenario) SCENARIO="$2"; shift ;;
        --all)      RUN_ALL=1 ;;
        --list)     grep -vE '^\s*(#|$)' "$PAIRS_FILE" | cut -d'|' -f1 | xargs -n1; exit 0 ;;
        -h|--help)  usage; exit 0 ;;
        *)          echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

[ -z "$SCENARIO" ] && [ "$RUN_ALL" -eq 0 ] && { usage >&2; exit 1; }

mkdir -p "$OUT"

while IFS='|' read -r name gen_host gen proc_host proc gen_lcores proc_lcores dur maxlat; do
    [[ "$name" =~ ^[[:space:]]*# ]] && continue
    [[ -z "${name// }" ]] && continue
    name=$(echo "$name" | xargs); gen_host=$(echo "$gen_host" | xargs)
    gen=$(echo "$gen" | xargs); proc_host=$(echo "$proc_host" | xargs); proc=$(echo "$proc" | xargs)
    gen_lcores=$(echo "$gen_lcores" | xargs); proc_lcores=$(echo "$proc_lcores" | xargs)
    dur=$(echo "$dur" | xargs); maxlat=$(echo "$maxlat" | xargs)
    [ -n "$SCENARIO" ] && [ "$name" != "$SCENARIO" ] && continue
    run_pair "$name" "$gen_host" "$gen" "$proc_host" "$proc" "$gen_lcores" "$proc_lcores" "$dur" "$maxlat"
done < "$PAIRS_FILE"

echo
echo "Results in: $OUT/"
echo "$PASSED passed, $FAILED failed (of $TOTAL)"
exit "$FAILED"
