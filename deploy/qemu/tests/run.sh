#!/bin/bash
# Automated test orchestrator for QEMU VM.
# Runs from the dev machine, SSHes into the QEMU VM.
#
# Usage: ./run.sh [--scenario <name>] [--all] [--deploy] [--host <ip>]

set -e

SCRIPT_PATH="$(dirname "$(realpath "$0")")"
DEVICE_DIR="$SCRIPT_PATH/.."
REPO_DIR="$DEVICE_DIR/../../"
SCENARIOS_FILE="$SCRIPT_PATH/scenarios.conf"
PARSE_SCRIPT="$DEVICE_DIR/../common/parse_results.sh"
RESULTS_DIR="$DEVICE_DIR/results"

HOST=192.168.21.2
USER=jarvis
PASS=123
REMOTE_DIR=/home/jarvis/pbus

DO_DEPLOY=0
RUN_ALL=0
SCENARIO_FILTER=""

SSH_CMD="ssh -o StrictHostKeyChecking=no ${USER}@${HOST}"
SCP_CMD="scp -o StrictHostKeyChecking=no"

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --scenario) SCENARIO_FILTER="$2"; shift ;;
        --all)      RUN_ALL=1 ;;
        --deploy)   DO_DEPLOY=1 ;;
        --host)     HOST="$2"; shift
                    SSH_CMD="ssh -o StrictHostKeyChecking=no ${USER}@${HOST}"
                    SCP_CMD="scp -o StrictHostKeyChecking=no" ;;
        *)          echo "Usage: $0 [--scenario <name>] [--all] [--deploy] [--host <ip>]"; exit 1 ;;
    esac
    shift
done

if [ -z "$SCENARIO_FILTER" ] && [ "$RUN_ALL" -eq 0 ]; then
    echo "Specify --scenario <name> or --all"
    echo ""
    echo "Available scenarios:"
    grep -v '^\s*#' "$SCENARIOS_FILE" | grep -v '^\s*$' | awk -F'|' '{gsub(/^[ \t]+|[ \t]+$/, "", $1); print "  " $1}'
    exit 1
fi

# Deploy if requested
if [ "$DO_DEPLOY" -eq 1 ]; then
    echo "=== Deploying ==="
    "$DEVICE_DIR/deploy.sh"
    echo ""
fi

# Read scenarios
TOTAL=0; PASSED=0; FAILED=0
DATE_DIR=$(date +%Y-%m-%d)
mkdir -p "$RESULTS_DIR/$DATE_DIR"

run_scenario() {
    local name="$1" gen_args="$2" proc_args="$3" proc_lcores="$4" duration="$5" max_latency="$6"
    local result_dir="$RESULTS_DIR/$DATE_DIR/$name"
    mkdir -p "$result_dir"

    TOTAL=$((TOTAL + 1))
    printf "[%d] %-25s (%dm%ds) " "$TOTAL" "$name" "$((duration/60))" "$((duration%60))"

    # Build runner args
    local runner_args="--gen-args '$gen_args' --proc-args '$proc_args' --duration $duration"
    runner_args="$runner_args --gen-script ./run_generator.sh --proc-script ./run_processor.sh"
    if [ -n "$proc_lcores" ]; then
        runner_args="$runner_args --proc-lcores '$proc_lcores'"
    fi

    # Run on remote
    $SSH_CMD "cd $REMOTE_DIR && sudo bash hw_test_runner.sh $runner_args" > /dev/null 2>&1

    # Fetch logs
    $SCP_CMD "${USER}@${HOST}:/tmp/gen.log" "$result_dir/generator.log" 2>/dev/null
    $SCP_CMD "${USER}@${HOST}:/tmp/proc.log" "$result_dir/processor.log" 2>/dev/null

    # Parse results
    bash "$PARSE_SCRIPT" \
        "$result_dir/generator.log" \
        "$result_dir/processor.log" \
        "$name" "$duration" "$max_latency" \
        > "$result_dir/summary.json"

    # Check pass/fail
    local pass=$(grep -o '"pass": [a-z]*' "$result_dir/summary.json" | awk '{print $2}')
    local max_us=$(grep -o '"max_us": [0-9]*' "$result_dir/summary.json" | head -1 | awk '{print $2}')
    local fail_reason=$(grep -o '"fail_reason": "[^"]*"' "$result_dir/summary.json" | cut -d'"' -f4)

    if [ "$pass" = "true" ]; then
        PASSED=$((PASSED + 1))
        echo "PASS  (max ${max_us}µs, 0 errors)"
    else
        FAILED=$((FAILED + 1))
        echo "FAIL  ($fail_reason)"
    fi
}

echo "=== Running Tests on $HOST (QEMU) ==="
echo ""

while IFS='|' read -r name gen_args proc_args proc_lcores duration max_latency; do
    # Skip comments and empty lines
    [[ "$name" =~ ^[[:space:]]*# ]] && continue
    [[ -z "$name" ]] && continue

    # Trim whitespace
    name=$(echo "$name" | xargs)
    gen_args=$(echo "$gen_args" | xargs)
    proc_args=$(echo "$proc_args" | xargs)
    proc_lcores=$(echo "$proc_lcores" | xargs)
    duration=$(echo "$duration" | xargs)
    max_latency=$(echo "$max_latency" | xargs)

    # Filter
    if [ -n "$SCENARIO_FILTER" ] && [ "$name" != "$SCENARIO_FILTER" ]; then
        continue
    fi

    run_scenario "$name" "$gen_args" "$proc_args" "$proc_lcores" "$duration" "$max_latency"
done < "$SCENARIOS_FILE"

echo ""
echo "Results: $RESULTS_DIR/$DATE_DIR/"
echo "$PASSED passed, $FAILED failed (of $TOTAL)"
exit $FAILED
