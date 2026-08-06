#!/bin/bash
# Runs generator + processor for a specified duration, then stops both.
# Deployed to target device and called via SSH from the test orchestrator.
#
# Usage: target_runner.sh --gen-args "<args>" --proc-args "<args>"
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
APPS='bus_generator|bus_processor'
STOP_DEADLINE=10

cleanup() {
    # Kill both if still running
    [ -n "$GEN_PID" ] && kill -INT "$GEN_PID" 2>/dev/null || true
    [ -n "$PROC_PID" ] && kill -INT "$PROC_PID" 2>/dev/null || true
    wait 2>/dev/null
}
trap cleanup EXIT

if pgrep -x "$APPS" > /dev/null; then
    echo "ERROR: apps from a previous run are still alive: $(pgrep -xa "$APPS")"
    exit 1
fi

rm -rf /var/run/dpdk/
rm -f "$GEN_LOG" "$PROC_LOG"

# Clean hugepages: unmount all layers, remount once
for mp in /mnt/bus_gen /mnt/bus_proc /mnt/delay_meter /mnt/redirect1 /mnt/redirect2; do
    while mountpoint -q "$mp" 2>/dev/null; do
        if ! umount "$mp" 2>/dev/null; then
            echo "ERROR: Cannot unmount $mp (device busy). Kill all DPDK processes first."
            exit 1
        fi
    done
    mkdir -p "$mp"
    mount -t hugetlbfs nodev "$mp"
done

echo "=== HW Test Runner ==="
echo "Generator:  $GEN_SCRIPT $GEN_ARGS"
echo "Processor:  $PROC_SCRIPT $PROC_ARGS"
echo "Duration:   ${DURATION}s"
echo ""

# Processor first, then the generator.
if [ ! -f "$PROC_SCRIPT" ]; then
    echo "ERROR: Processor script not found: $PROC_SCRIPT"
    exit 1
fi

LCORES="$PROC_LCORES" $PROC_SCRIPT $PROC_ARGS --time $((DURATION + 10)) > "$PROC_LOG" 2>&1 &
PROC_PID=$!
echo "Processor started (PID=$PROC_PID)"

# EAL init, port configure and link-up take a few seconds; without this the
# generator would still win the race.
sleep 5

$GEN_SCRIPT $GEN_ARGS --time "$DURATION" > "$GEN_LOG" 2>&1 &
GEN_PID=$!
echo "Generator started (PID=$GEN_PID)"

# Verify both started (give them a moment to load)
sleep 2
if ! kill -0 "$GEN_PID" 2>/dev/null; then
    echo "ERROR: Generator failed to start. Check $GEN_LOG"
    cat "$GEN_LOG"
    exit 1
fi
if ! kill -0 "$PROC_PID" 2>/dev/null; then
    echo "ERROR: Processor failed to start. Check $PROC_LOG"
    cat "$PROC_LOG"
    kill -INT "$GEN_PID" 2>/dev/null || true
    exit 1
fi

echo "Running for ${DURATION}s..."
wait "$GEN_PID" 2>/dev/null || true
wait "$PROC_PID" 2>/dev/null || true

sudo killall -9 bus_generator bus_processor 2>/dev/null || true
for _ in $(seq "$STOP_DEADLINE"); do
    pgrep -x "$APPS" > /dev/null || break
    sleep 1
done
if pgrep -x "$APPS" > /dev/null; then
    echo "ERROR: apps survived kill -9: $(pgrep -xa "$APPS")"
    exit 1
fi

echo "Done. Logs: $GEN_LOG $PROC_LOG"
