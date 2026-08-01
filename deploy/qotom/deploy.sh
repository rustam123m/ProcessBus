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
INSTALL_DIR="$REPO_DIR/install-atom"
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
RUN_ALL=0
SCENARIO=""

usage() {
    cat <<EOF
Usage: $0 [--rebuild] [--scenario <name> | --all] [--host <addr>]

Default behaviour: push files only (no rebuild, no scenarios).

  --rebuild          Rebuild via ci/build.sh --rebuild before pushing
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
        --scenario) SCENARIO="$2"; shift ;;
        --all)      RUN_ALL=1 ;;
        --host)     HOST="$2"; shift ;;
        -h|--help)  usage; exit 0 ;;
        *)          echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

SSH="ssh -o StrictHostKeyChecking=no $USER@$HOST"
SCP="scp -o StrictHostKeyChecking=no"

# --- 1. Optional rebuild ---
if [ "$DO_REBUILD" -eq 1 ]; then
    echo "=== Rebuilding ==="
    "$REPO_DIR/ci/build.sh" --rebuild
fi

# --- 2. Bundle and push ---
echo "=== Pushing files to $USER@$HOST:$REMOTE_DIR ==="
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
cp -rp "$INSTALL_DIR/"* "$BUNDLE/"
cp -p  "$COMMON_DIR/target_runner.sh" "$BUNDLE/"
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

# --- 4. Run scenarios (if requested) ---
if [ -z "$SCENARIO" ] && [ "$RUN_ALL" -eq 0 ]; then
    echo "=== Done (no scenario requested) ==="
    exit 0
fi

DATE_DIR=$(date +%Y-%m-%d)
mkdir -p "$RESULTS_DIR/$DATE_DIR"
TOTAL=0; PASSED=0; FAILED=0

run_scenario() {
    local name="$1" gen_args="$2" proc_args="$3" proc_lcores="$4" duration="$5" max_lat="$6"
    local out="$RESULTS_DIR/$DATE_DIR/$name"
    mkdir -p "$out"
    TOTAL=$((TOTAL + 1))

    local mins=$((duration / 60)) secs=$((duration % 60))
    echo
    echo "==== [$TOTAL] $name  (${mins}m${secs}s) ============================"

    local args="--gen-args '$gen_args' --proc-args '$proc_args' --duration $duration"
    args="$args --gen-script ./run_generator.sh --proc-script ./run_processor.sh"
    [ -n "$proc_lcores" ] && args="$args --proc-lcores '$proc_lcores'"

    # Stream remote output indented; don't fail the whole deploy if a scenario errors.
    $SSH "cd $REMOTE_DIR && sudo bash target_runner.sh $args" 2>&1 | sed 's/^/  | /' || true
    $SCP "$USER@$HOST:/tmp/gen.log"  "$out/generator.log" 2>/dev/null || true
    $SCP "$USER@$HOST:/tmp/proc.log" "$out/processor.log" 2>/dev/null || true

    bash "$PARSE_SCRIPT" "$out/generator.log" "$out/processor.log" \
        "$name" "$duration" "$max_lat" > "$out/summary.json"

    local pass max_us why
    pass=$(grep -o '"pass": [a-z]*'        "$out/summary.json" | awk '{print $2}')
    max_us=$(grep -o '"max_us": [0-9]*'    "$out/summary.json" | head -1 | awk '{print $2}')
    why=$(grep -o '"fail_reason": "[^"]*"' "$out/summary.json" | cut -d'"' -f4)

    if [ "$pass" = "true" ]; then
        PASSED=$((PASSED + 1))
        echo "==== [$TOTAL] $name: PASS  (max ${max_us}us)"
    else
        FAILED=$((FAILED + 1))
        echo "==== [$TOTAL] $name: FAIL  ($why)"
    fi
}

echo "=== Running scenarios on $HOST ==="
while IFS='|' read -r name gen proc lcores dur maxlat; do
    [[ "$name" =~ ^[[:space:]]*# ]] && continue
    [[ -z "$name" ]] && continue
    name=$(echo   "$name"   | xargs)
    gen=$(echo    "$gen"    | xargs)
    proc=$(echo   "$proc"   | xargs)
    lcores=$(echo "$lcores" | xargs)
    dur=$(echo    "$dur"    | xargs)
    maxlat=$(echo "$maxlat" | xargs)
    [ -n "$SCENARIO" ] && [ "$name" != "$SCENARIO" ] && continue
    run_scenario "$name" "$gen" "$proc" "$lcores" "$dur" "$maxlat"
done < "$SCENARIOS_FILE"

echo
echo "Results in: $RESULTS_DIR/$DATE_DIR/"
echo "$PASSED passed, $FAILED failed (of $TOTAL)"
exit "$FAILED"
