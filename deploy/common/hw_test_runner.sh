#!/bin/bash
# Runs generator + processor for a specified duration, then stops both.
# Deployed to target device and called via SSH from the test orchestrator.
#
# Usage: hw_test_runner.sh --gen-args "<args>" --proc-args "<args>"
#                          --proc-lcores "<lcores>" --duration <sec>
#                          --gen-script <path> --proc-script <path>

set -e

GEN_ARGS=""
PROC_ARGS=""
PROC_LCORES=""
DURATION=60
GEN_SCRIPT="./run_generator.sh"
PROC_SCRIPT="./run_processor.sh"

while [[ "$#" -gt 0 ]]; do
    case $1 in
        --gen-args)    GEN_ARGS="$2"; shift ;;
        --proc-args)   PROC_ARGS="$2"; shift ;;
        --proc-lcores) PROC_LCORES="$2"; shift ;;
        --duration)    DURATION="$2"; shift ;;
        --gen-script)  GEN_SCRIPT="$2"; shift ;;
        --proc-script) PROC_SCRIPT="$2"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

GEN_LOG="/tmp/gen.log"
PROC_LOG="/tmp/proc.log"

cleanup() {
    # Kill both if still running
    [ -n "$GEN_PID" ] && kill -INT "$GEN_PID" 2>/dev/null || true
    [ -n "$PROC_PID" ] && kill -INT "$PROC_PID" 2>/dev/null || true
    wait 2>/dev/null
}
trap cleanup EXIT

echo "=== HW Test Runner ==="
echo "Generator:  $GEN_SCRIPT $GEN_ARGS"
echo "Processor:  $PROC_SCRIPT $PROC_ARGS"
echo "Duration:   ${DURATION}s"
echo ""

# Start generator
$GEN_SCRIPT $GEN_ARGS > "$GEN_LOG" 2>&1 &
GEN_PID=$!
echo "Generator started (PID=$GEN_PID)"

# Start processor (override lcores if specified)
if [ -n "$PROC_LCORES" ]; then
    # Replace the -l argument in the script by passing DPDK EAL args
    # The run_processor.sh passes $@ after --, so we prepend lcore override
    # Actually, we need to call bus_processor directly with custom lcores
    PROC_SCRIPT_DIR="$(dirname "$PROC_SCRIPT")"
    source_nic=$(grep "NIC_PCI_ADDR" "$PROC_SCRIPT" | head -1 | grep -v "^#" | cut -d'"' -f2)
    hugedir=$(grep "huge-dir" "$PROC_SCRIPT" | head -1 | grep -oP '/mnt/[^ /]+/')
    prefix=$(grep "file-prefix" "$PROC_SCRIPT" | head -1 | grep -oP 'file-prefix=\K[^ ]+')

    bin/bus_processor -l "$PROC_LCORES" \
        -a "$source_nic" \
        --huge-dir="$hugedir" \
        --file-prefix="$prefix" \
        -- $PROC_ARGS > "$PROC_LOG" 2>&1 &
    PROC_PID=$!
else
    $PROC_SCRIPT $PROC_ARGS > "$PROC_LOG" 2>&1 &
    PROC_PID=$!
fi
echo "Processor started (PID=$PROC_PID)"

# Wait for test duration
echo "Running for ${DURATION}s..."
sleep "$DURATION"

# Stop both gracefully
echo "Stopping..."
kill -INT "$GEN_PID" 2>/dev/null || true
kill -INT "$PROC_PID" 2>/dev/null || true

# Wait for clean exit
wait "$GEN_PID" 2>/dev/null || true
wait "$PROC_PID" 2>/dev/null || true

echo "Done. Logs: $GEN_LOG $PROC_LOG"
