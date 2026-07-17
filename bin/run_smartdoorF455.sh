#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROGDIR="$SCRIPT_DIR"
PROG="$PROGDIR/smartdoorF455"
# Logs live one level above the binary (project root), not inside bin/.
LOGDIR="$(cd "$SCRIPT_DIR/.." && pwd)/log"
PIDFILE="$LOGDIR/smartdoorF455.pid"
CPU_AFFINITY=3
CONFIG_PATH="$PROGDIR/config.toml"
TASKSET_ENABLED=true

usage() {
    cat <<EOF
Usage: $(basename "$0") {start|stop|status|restart|foreground} [--config PATH] [--logdir PATH] [--no-taskset]

Commands:
  start       Start smartdoorF455 in the background
  stop        Stop the running smartdoorF455 process
  status      Show whether smartdoorF455 is running
  restart     Stop and then start smartdoorF455
  foreground  Run smartdoorF455 in the foreground

Options:
  --config PATH    Pass a custom config.toml path to smartdoorF455
  --logdir PATH    Write logs and PID file to a custom directory
  --no-taskset     Do not set CPU affinity after startup
EOF
}

required_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: required command not found: $1" >&2
        exit 1
    fi
}

pidfile_running_pid() {
    if [ -r "$PIDFILE" ]; then
        local pid
        pid=$(<"$PIDFILE")
        if [ -n "$pid" ] && kill -0 "$pid" >/dev/null 2>&1; then
            printf '%s\n' "$pid"
            return 0
        fi
        rm -f "$PIDFILE"
    fi
    return 1
}

running_pids() {
    if pidfile_running_pid >/dev/null 2>&1; then
        printf '%s\n' "$(<"$PIDFILE")"
        return 0
    fi

    local pids
    pids=$(pgrep -u "$(id -u)" -x "$(basename "$PROG")" || true)
    if [ -n "$pids" ]; then
        printf '%s\n' "$pids"
        return 0
    fi
    return 1
}

ensure_logdir() {
    mkdir -p "$LOGDIR"
}

ensure_program_exists() {
    if [ ! -x "$PROG" ]; then
        echo "Error: executable not found: $PROG" >&2
        echo "Please build smartdoorF455 first and retry." >&2
        exit 1
    fi
}

set_nice_capability() {
    if ! command -v getcap >/dev/null 2>&1 || ! command -v setcap >/dev/null 2>&1; then
        return
    fi

    if ! getcap "$PROG" 2>/dev/null | grep -q 'cap_sys_nice'; then
        echo "Setting cap_sys_nice on $PROG (may require sudo)"
        if command -v sudo >/dev/null 2>&1; then
            sudo setcap 'cap_sys_nice=eip' "$PROG" || echo "Warning: failed to set cap_sys_nice" >&2
        else
            echo "Warning: sudo not available; skipping setcap" >&2
        fi
    fi
}

set_cpu_affinity() {
    if [ "$TASKSET_ENABLED" != true ]; then
        return
    fi
    if ! command -v taskset >/dev/null 2>&1; then
        return
    fi
    local pid="$1"
    if [ "$(id -u)" -eq 0 ]; then
        taskset -cp "$CPU_AFFINITY" "$pid" >/dev/null 2>&1 || echo "Warning: failed to set CPU affinity for PID $pid" >&2
    elif command -v sudo >/dev/null 2>&1; then
        sudo taskset -cp "$CPU_AFFINITY" "$pid" >/dev/null 2>&1 || echo "Warning: failed to set CPU affinity for PID $pid" >&2
    else
        echo "Warning: sudo unavailable; cannot set CPU affinity" >&2
    fi
}

stop_program() {
    if ! running_pids >/dev/null 2>&1; then
        echo "smartdoorF455 is not running."
        return 0
    fi

    local pids
    pids=$(running_pids)
    echo "Stopping smartdoorF455 PIDs:"
    echo "$pids"

    local pid
    while IFS= read -r pid; do
        kill "$pid" >/dev/null 2>&1 || true
    done <<< "$pids"

    for i in {1..10}; do
        if ! running_pids >/dev/null 2>&1; then
            break
        fi
        sleep 1
    done

    if running_pids >/dev/null 2>&1; then
        echo "smartdoorF455 did not stop cleanly; sending SIGKILL"
        while IFS= read -r pid; do
            kill -9 "$pid" >/dev/null 2>&1 || true
        done <<< "$(running_pids)"
    fi

    rm -f "$PIDFILE"
    echo "Stopped smartdoorF455."
}

status_program() {
    if running_pids >/dev/null 2>&1; then
        echo "smartdoorF455 is running with PID(s):"
        running_pids
        return 0
    fi
    echo "smartdoorF455 is not running."
    return 1
}

parse_options() {
    local args=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --config)
                shift
                if [ $# -eq 0 ]; then
                    echo "Error: --config requires a path" >&2
                    exit 1
                fi
                CONFIG_PATH="$1"
                ;;
            --logdir)
                shift
                if [ $# -eq 0 ]; then
                    echo "Error: --logdir requires a path" >&2
                    exit 1
                fi
                LOGDIR="$1"
                PIDFILE="$LOGDIR/$(basename "$PIDFILE")"
                ;;
            --no-taskset)
                TASKSET_ENABLED=false
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
                args+=("$1")
                ;;
        esac
        shift
    done
    if [ ${#args[@]} -gt 0 ]; then
        COMMAND="${args[0]}"
    fi
}

start_program() {
    if running_pids >/dev/null 2>&1; then
        echo "smartdoorF455 is already running:"
        running_pids
        exit 1
    fi

    ensure_program_exists
    ensure_logdir

    set_nice_capability

    local logfile
    logfile="$LOGDIR/$(date +'%Y%m%d_%H%M%S')_smartdoorF455.log"
    echo "Starting smartdoorF455..."

    cd "$PROGDIR"
    nohup "$PROG" "$CONFIG_PATH" </dev/null >>"$logfile" 2>&1 &
    local pid=$!
    echo "$pid" > "$PIDFILE"
    echo "Started smartdoorF455 with PID $pid"
    echo "Logging to $logfile"
    set_cpu_affinity "$pid"
}

run_foreground() {
    ensure_program_exists
    cd "$PROGDIR"
    exec "$PROG" "$CONFIG_PATH"
}

if [ $# -lt 1 ]; then
    usage
    exit 1
fi

COMMAND="$1"
shift
parse_options "$@"

case "$COMMAND" in
    start)
        required_command pgrep
        required_command nohup
        start_program
        ;;
    stop)
        required_command pgrep
        stop_program
        ;;
    status)
        required_command pgrep
        status_program
        ;;
    restart)
        required_command pgrep
        required_command nohup
        stop_program
        start_program
        ;;
    foreground)
        run_foreground
        ;;
    *)
        usage
        exit 1
        ;;
esac
