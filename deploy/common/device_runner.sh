#!/bin/bash
# On-device runner for ONE role (generator or processor). The HOST launches this
# script inside a detached tmux session; this script then runs the app in the
# FOREGROUND and owns its lifecycle. It is the top-level device script that the
# harness rules refer to.
#
# Harness rules (see perf plan "HARNESS RULES"):
#  1. Graceful stop: SIGTERM first; SIGKILL only after a timeout (--action stop).
#  2. No random child-hunting: stop only the named app, by exact name.
#  3. No '&': the app runs in the foreground of this script; this script itself
#     runs detached because the HOST puts it in a tmux session.
#  4. Capture on device: app output -> <outdir>/<name>_<role>.
#  5. Pre-flight idle check (--action preflight) before any run.
#
# Actions:
#   preflight                              stop both apps, clean hugepages
#   run   --role R --name N --args "..."   run the app in foreground (host wraps
#         --lcores L --nic PCI --duration S  this in tmux); returns when the app
#         --outdir D [--time-pad P]          exits (its --time) or is stopped
#   wait  --role R                         block until the role's tmux session ends
#   stop  --role R                         SIGTERM the role's app, then SIGKILL

set -u

ACTION="" ROLE="" NAME="" APP_ARGS="" LCORES="" NIC="" DURATION=60
OUTDIR="/tmp/pbus_out" ROLE_SCRIPT="" TIME_PAD=0
export USE_AF_PACKET="${USE_AF_PACKET:-0}"
export HOST_IFACE="${HOST_IFACE:-end0}"
STOP_DEADLINE=10
UMOUNT_TRIES=20      # x UMOUNT_WAIT = how long to wait for a busy hugetlbfs
UMOUNT_WAIT=0.5

# Positional form used by run_tests.py: "<action> [role] [name]". Keeps the ssh
# line to one script with at most one meaningful argument -- the run parameters
# come from runs/<name>_<role>.params, copied in beforehand, not from the command
# line. The --action form below is kept for the older shell harnesses.
if [ "$#" -gt 0 ] && [ "${1#--}" = "$1" ]; then
    cd "$(dirname "$(realpath "$0")")" || exit 1
    ACTION="$1"; ROLE="${2:-}"; NAME="${3:-}"
    if [ "$ACTION" = run ]; then
        PARAMS="runs/${NAME}_${ROLE}.params"
        [ -f "$PARAMS" ] || { echo "ERROR: missing $PARAMS" >&2; exit 2; }
        # Sets APP_ARGS, LCORES, NIC and TIME for this role.
        . "./$PARAMS"
        DURATION="$TIME"; TIME_PAD=0
    fi
    set --
fi

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --action)      ACTION="$2"; shift ;;
        --role)        ROLE="$2"; shift ;;
        --name)        NAME="$2"; shift ;;
        --args)        APP_ARGS="$2"; shift ;;
        --lcores)      LCORES="$2"; shift ;;
        --nic)         NIC="$2"; shift ;;
        --duration)    DURATION="$2"; shift ;;
        --outdir)      OUTDIR="$2"; shift ;;
        --role-script) ROLE_SCRIPT="$2"; shift ;;
        --time-pad)    TIME_PAD="$2"; shift ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

app_of()         { [ "$1" = gen ] && echo bus_generator || echo bus_processor; }
session_of()     { echo "pbus_$1"; }
default_script() { [ "$1" = gen ] && echo ./run_generator.sh || echo ./run_processor.sh; }

# Rule 1: SIGTERM then, only after a timeout, SIGKILL. Targets one app by name.
graceful_stop_app() {
    local app="$1" waited=0
    pgrep -x "$app" >/dev/null || return 0
    echo "device_runner: SIGTERM $app"
    pkill -TERM -x "$app" 2>/dev/null || true
    while [ "$waited" -lt "$STOP_DEADLINE" ]; do
        pgrep -x "$app" >/dev/null || return 0
        sleep 1; waited=$((waited + 1))
    done
    echo "WARN: $app ignored SIGTERM after ${STOP_DEADLINE}s; SIGKILL" >&2
    pkill -KILL -x "$app" 2>/dev/null || true
    sleep 1
    pgrep -x "$app" >/dev/null && { echo "ERROR: $app survived SIGKILL" >&2; return 1; }
    return 0
}

clean_hugepages() {
    rm -rf /var/run/dpdk/ 2>/dev/null || true
    local mp tries err
    for mp in /mnt/bus_gen /mnt/bus_proc /mnt/delay_meter /mnt/redirect1 /mnt/redirect2; do
        while mountpoint -q "$mp" 2>/dev/null; do
            # The kernel releases hugetlbfs mappings asynchronously, so umount
            # can return EBUSY for a moment after the app died - especially
            # after a SIGKILL or an early stop. Retry briefly instead of
            # aborting the whole batch on the first EBUSY, and report the real
            # umount error if it never clears.
            tries=0
            until err=$(umount "$mp" 2>&1); do
                tries=$((tries + 1))
                if [ "$tries" -ge "$UMOUNT_TRIES" ]; then
                    echo "ERROR: cannot unmount $mp after $tries tries: $err" >&2
                    return 1
                fi
                sleep "$UMOUNT_WAIT"
            done
        done
        mkdir -p "$mp"
        mount -t hugetlbfs nodev "$mp" 2>/dev/null || true
    done
}

case "$ACTION" in
    preflight)
        # Rule 5: nothing may be running before a scenario starts.
        graceful_stop_app bus_generator || exit 1
        graceful_stop_app bus_processor || exit 1
        clean_hugepages || exit 1
        echo "device_runner: preflight clean"
        ;;

    run)
        [ -n "$ROLE" ] && [ -n "$NAME" ] || { echo "ERROR: run needs --role and --name" >&2; exit 2; }
        APP="$(app_of "$ROLE")"
        [ -n "$ROLE_SCRIPT" ] || ROLE_SCRIPT="$(default_script "$ROLE")"
        mkdir -p "$OUTDIR"
        OUTFILE="$OUTDIR/${NAME}_${ROLE}"
        rm -f "$OUTFILE"
        # However this script exits (normal, or SIGTERM'd by the host), make sure
        # the app it started is not left behind. Role-scoped: never touches the
        # other role, so single-box keeps the processor alive while gen starts.
        trap 'graceful_stop_app "$APP"' EXIT
        graceful_stop_app "$APP" || exit 1
        # Rule 3: no '&'. The app runs in the foreground; this script is already
        # detached because the host launched it inside a tmux session. The app's
        # own --time bounds the run; --action stop ends it early if needed.
        LCORES="$LCORES" NIC_PCI_ADDR="$NIC" \
            bash "$ROLE_SCRIPT" $APP_ARGS --time $((DURATION + TIME_PAD)) > "$OUTFILE" 2>&1
        RC=$?
        # The caller reads this from the collected log: a crash or a kill is
        # otherwise invisible, since the shell's own notice dies with the pane.
        echo "device_runner: exit_status=$RC" >> "$OUTFILE"
        echo "device_runner: $ROLE finished -> $OUTFILE"
        ;;

    wait)
        [ -n "$ROLE" ] || { echo "ERROR: wait needs --role" >&2; exit 2; }
        SESSION="$(session_of "$ROLE")"
        DEADLINE=$((DURATION + TIME_PAD + STOP_DEADLINE + 5)) waited=0
        while [ "$waited" -lt "$DEADLINE" ]; do
            tmux has-session -t "$SESSION" 2>/dev/null || { echo "device_runner: $ROLE done"; exit 0; }
            sleep 1; waited=$((waited + 1))
        done
        echo "WARN: $ROLE still running after ${DEADLINE}s" >&2
        exit 0
        ;;

    stop)
        [ -n "$ROLE" ] || { echo "ERROR: stop needs --role" >&2; exit 2; }
        graceful_stop_app "$(app_of "$ROLE")" || exit 1
        # The app is gone, but the run instance still has to record the exit
        # status and close. Killing the session right here truncated the log and
        # made a deliberate stop look like a crash. Give it a moment to leave.
        SESSION="$(session_of "$ROLE")"
        waited=0
        while [ "$waited" -lt 5 ]; do
            tmux has-session -t "$SESSION" 2>/dev/null || break
            sleep 1; waited=$((waited + 1))
        done
        tmux kill-session -t "$SESSION" 2>/dev/null || true
        echo "device_runner: $ROLE stopped"
        ;;

    *) echo "ERROR: --action must be preflight|run|wait|stop" >&2; exit 2 ;;
esac
