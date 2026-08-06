#!/bin/bash
# Cross-device scenarios from pairs.conf: generator on one board, processor on
# the other. Deploy to both boards first; this script only runs them.

set -e

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
REPO_DIR="$SCRIPT_DIR/../.."
RESULTS_DIR="$SCRIPT_DIR/results"
PAIRS_FILE="$SCRIPT_DIR/pairs.conf"
PARSE_SCRIPT="$REPO_DIR/deploy/common/result_parser.sh"

START_DELAY=15
START_DEADLINE=30
STOP_DEADLINE=30
APPS='bus_generator|bus_processor'

RUN_ALL=0
SCENARIO=""

usage() {
    cat <<EOF
Usage: $0 [--scenario <name> | --all] [--list]

  --scenario <name>  Run a single scenario from pairs.conf
  --all              Run every scenario in pairs.conf
  --list             Print scenario names and exit
  -h, --help         Show this help
EOF
}

function host_config()
{
    case "$1" in
        blackbox)
            HOST_ADDR="192.168.100.103"
            HOST_DIR="/home/jarvis/pbus"
            HOST_NIC="0000:07:00.0"
            ;;
        opi3b)
            HOST_ADDR="192.168.100.105"
            HOST_DIR="/home/jarvis/pbus"
            HOST_NIC="0000:01:00.0"
            ;;
        *)
            echo "Unknown host: $1" >&2
            exit 1
            ;;
    esac
}

# ConnectTimeout bounds the connect only; ServerAlive bounds a wedged session.
function remote()
{
    ssh -n -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "jarvis@$1" "$2"
}

function apps_running()
{
    local count
    if ! count=$(remote "$1" "pgrep -xc '$APPS' || true"); then
        echo "ERROR: $1 is unreachable" >&2
        exit 1
    fi
    [ "${count:-0}" -gt 0 ]
}

# A running board is not pollable over SSH: RT pollers starve sshd.
function wait_log()
{
    local file="$1" pattern="$2" waited=0
    while [ "$waited" -lt "$START_DEADLINE" ]; do
        grep -q "$pattern" "$file" 2>/dev/null && return 0
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

function wait_pid()
{
    local pid="$1" limit="$2" waited=0
    while [ "$waited" -lt "$limit" ]; do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

function wait_stopped()
{
    local addr="$1" limit="$2" waited=0
    while [ "$waited" -lt "$limit" ]; do
        apps_running "$addr" || return 0
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

function require_idle()
{
    local addr="$1"
    if apps_running "$addr"; then
        echo "ERROR: $addr is still running an app: $(remote "$addr" "pgrep -xa '$APPS'")" >&2
        exit 1
    fi
    # A SIGKILLed run leaves its hugepage maps behind and they are never reused.
    remote "$addr" "sudo rm -rf /var/run/dpdk/
        sudo rm -f /dev/hugepages/bus_* /mnt/bus_gen/bus_* /mnt/bus_proc/bus_*
        for mp in /mnt/bus_gen /mnt/bus_proc; do
            [ -d \"\$mp\" ] || continue
            mountpoint -q \"\$mp\" || sudo mount -t hugetlbfs nodev \"\$mp\"
        done" >/dev/null 2>&1 || true
}

function force_stop()
{
    local addr="$1"
    remote "$addr" "sudo killall -9 bus_generator bus_processor 2>/dev/null; true" >/dev/null
    if ! wait_stopped "$addr" "$STOP_DEADLINE"; then
        echo "ERROR: $addr survived kill -9: $(remote "$addr" "pgrep -xa '$APPS'")" >&2
        exit 1
    fi
    echo "$addr: stopped"
}

function stop_all()
{
    local host
    for host in blackbox opi3b; do
        host_config "$host"
        force_stop "$HOST_ADDR"
    done
}

DATE_DIR=$(date +%Y-%m-%d)
TOTAL=0; PASSED=0; FAILED=0

run_pair() {
    local name="$1" gen_host="$2" gen_args="$3" proc_host="$4" proc_args="$5"
    local proc_lcores="$6" duration="$7" max_lat="$8"
    local out="$RESULTS_DIR/$DATE_DIR/$name"
    mkdir -p "$out"
    rm -f "$out"/*.log
    TOTAL=$((TOTAL + 1))

    host_config "$gen_host"
    local gen_addr="$HOST_ADDR" gen_dir="$HOST_DIR" gen_nic="$HOST_NIC"
    host_config "$proc_host"
    local proc_addr="$HOST_ADDR" proc_dir="$HOST_DIR" proc_nic="$HOST_NIC"

    echo
    echo "==== [$TOTAL] $name  ($gen_host -> $proc_host, ${duration}s) ===="

    require_idle "$gen_addr"
    require_idle "$proc_addr"

    remote "$proc_addr" \
        "cd $proc_dir && sudo env NIC_PCI_ADDR=$proc_nic LCORES='$proc_lcores' \
         bash run_processor.sh $proc_args --time $((duration + START_DELAY + 5))" \
        > "$out/processor.log" 2>&1 &
    local proc_ssh=$!
    if ! wait_log "$out/processor.log" "EAL:"; then
        echo "ERROR: processor did not start on $proc_host"
        tail -25 "$out/processor.log"
        stop_all
        FAILED=$((FAILED + 1))
        return 0
    fi

    sleep "$START_DELAY"

    remote "$gen_addr" \
        "cd $gen_dir && sudo env NIC_PCI_ADDR=$gen_nic \
         bash run_generator.sh $gen_args --time $duration" \
        > "$out/generator.log" 2>&1 &
    local gen_ssh=$!
    if ! wait_log "$out/generator.log" "EAL:"; then
        echo "ERROR: generator did not start on $gen_host"
        tail -25 "$out/generator.log"
        stop_all
        FAILED=$((FAILED + 1))
        return 0
    fi

    if ! wait_pid "$gen_ssh" $((duration + STOP_DEADLINE)); then
        echo "ERROR: generator on $gen_host ignored --time" >&2
        stop_all
        exit 1
    fi
    if ! wait_pid "$proc_ssh" $((START_DELAY + STOP_DEADLINE)); then
        echo "ERROR: processor on $proc_host ignored --time" >&2
        stop_all
        exit 1
    fi

    local err
    err=$(grep -hm1 'Exception:\|EAL: Error' "$out"/*.log || true)
    if [ -n "$err" ]; then
        FAILED=$((FAILED + 1))
        echo "==== [$TOTAL] $name: FAIL  ($err)"
        return 0
    fi

    bash "$PARSE_SCRIPT" "$out/generator.log" "$out/processor.log" \
        "$name" "$duration" "$max_lat" > "$out/summary.json"

    local pass why
    pass=$(grep -o '"pass": [a-z]*'        "$out/summary.json" | awk '{print $2}')
    why=$(grep -o '"fail_reason": "[^"]*"' "$out/summary.json" | cut -d'"' -f4)

    if [ "$pass" = "true" ]; then
        PASSED=$((PASSED + 1))
        echo "==== [$TOTAL] $name: PASS"
    else
        FAILED=$((FAILED + 1))
        echo "==== [$TOTAL] $name: FAIL  ($why)"
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

if [ -z "$SCENARIO" ] && [ "$RUN_ALL" -eq 0 ]; then
    usage >&2
    exit 1
fi

mkdir -p "$RESULTS_DIR/$DATE_DIR"

while IFS='|' read -r name gen_host gen proc_host proc lcores dur maxlat; do
    [[ "$name" =~ ^[[:space:]]*# ]] && continue
    [[ -z "${name// }" ]] && continue
    name=$(echo      "$name"      | xargs)
    gen_host=$(echo  "$gen_host"  | xargs)
    gen=$(echo       "$gen"       | xargs)
    proc_host=$(echo "$proc_host" | xargs)
    proc=$(echo      "$proc"      | xargs)
    lcores=$(echo    "$lcores"    | xargs)
    dur=$(echo       "$dur"       | xargs)
    maxlat=$(echo    "$maxlat"    | xargs)
    [ -n "$SCENARIO" ] && [ "$name" != "$SCENARIO" ] && continue
    run_pair "$name" "$gen_host" "$gen" "$proc_host" "$proc" "$lcores" "$dur" "$maxlat"
done < "$PAIRS_FILE"

echo
stop_all

echo
echo "Results in: $RESULTS_DIR/$DATE_DIR/"
echo "$PASSED passed, $FAILED failed (of $TOTAL)"
exit "$FAILED"
