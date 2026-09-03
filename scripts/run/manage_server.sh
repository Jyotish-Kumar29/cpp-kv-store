#!/usr/bin/env bash

set -euo pipefail

# Single entry point for kv-store server lifecycle.
# Other project scripts should start and stop the server through this script.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

BINARY="$ROOT/build/kvstore"
PIDFILE="/tmp/kvstore_server.pid"
PORT="${2:-8080}"
PERSISTENT="${3:-true}"

STOP_WAIT_TICKS=50   # 5 seconds
PORT_WAIT_TICKS=50   # 5 seconds

RED=$'\033[0;31m'
GREEN=$'\033[0;32m'
YELLOW=$'\033[1;33m'
NC=$'\033[0m'

check_binary() {
    if [[ ! -x "$BINARY" ]]; then
        printf '%sERROR: Server binary not found or not executable at %s%s\n' \
            "$RED" "$BINARY" "$NC" >&2
        printf 'Run ./scripts/build/build_server.sh first.\n' >&2
        exit 1
    fi
}

port_in_use() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn "( sport = :$PORT )" 2>/dev/null | grep -q ":$PORT"
    elif command -v lsof >/dev/null 2>&1; then
        lsof -i ":$PORT" -sTCP:LISTEN >/dev/null 2>&1
    else
        # Fallback when ss/lsof are unavailable.
        if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi

        return 1
    fi
}

wait_for_pid_exit() {
    local pid="$1"

    for ((i = 0; i < STOP_WAIT_TICKS; ++i)); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
    done

    return 1
}

wait_for_port_free() {
    for ((i = 0; i < PORT_WAIT_TICKS; ++i)); do
        port_in_use || return 0
        sleep 0.1
    done

    return 1
}

case "$1" in
start)
    check_binary

    if [[ -f "$PIDFILE" ]] &&
       kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        printf 'Server already running (PID %s)\n' "$(cat "$PIDFILE")"
        exit 1
    fi

    if port_in_use; then
        printf '%sERROR: Port %s is already in use by another process (no matching PID file).%s\n' \
            "$RED" "$PORT" "$NC" >&2
        printf 'Check with: ss -ltnp | grep %s   (or) lsof -i :%s\n' \
            "$PORT" "$PORT" >&2
        exit 1
    fi

    printf 'Starting server on port %s (persistent=%s)...\n' "$PORT" "$PERSISTENT"

    "$BINARY" "$PORT" "$PERSISTENT" &

    NEW_PID=$!
    printf '%s\n' "$NEW_PID" > "$PIDFILE"

    # Verify that the process survives startup and successfully binds the port.
    for ((i = 0; i < 20; ++i)); do
        if ! kill -0 "$NEW_PID" 2>/dev/null; then
            printf '%sERROR: Server exited immediately after start (check binary output).%s\n' \
                "$RED" "$NC" >&2
            rm -f "$PIDFILE"
            exit 1
        fi

        if port_in_use; then
            printf 'Started (PID %s)\n' "$NEW_PID"
            exit 0
        fi

        sleep 0.1
    done

    printf '%sERROR: Server process is running but never bound to port %s.%s\n' \
        "$RED" "$PORT" "$NC" >&2
    exit 1
    ;;

stop)
    if [[ ! -f "$PIDFILE" ]]; then
        printf 'No PID file found\n'
        exit 1
    fi

    PID="$(cat "$PIDFILE")"

    if kill -0 "$PID" 2>/dev/null; then
        printf 'Stopping server (PID %s)...\n' "$PID"

        kill "$PID"

        if ! wait_for_pid_exit "$PID"; then
            printf 'Process did not exit gracefully within 5s, sending SIGKILL...\n'

            kill -9 "$PID" 2>/dev/null

            if ! wait_for_pid_exit "$PID"; then
                printf '%sWARNING: PID %s still not confirmed dead.%s\n' \
                    "$YELLOW" "$PID" "$NC"
            fi
        fi

        # Wait for the listening socket to disappear before allowing a new start.
        if ! wait_for_port_free; then
            printf '%sWARNING: port %s still appears to be in use after stop.%s\n' \
                "$YELLOW" "$PORT" "$NC"
        fi

        rm -f "$PIDFILE"
        printf 'Stopped\n'
    else
        printf 'Process not running\n'
        rm -f "$PIDFILE"
    fi
    ;;

status)
    if [[ -f "$PIDFILE" ]] &&
       kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        printf 'Running (PID %s)\n' "$(cat "$PIDFILE")"
    else
        printf 'Not running\n'
    fi
    ;;

*)
    printf 'Usage: %s {start|stop|status} [port] [persistent(true|false)]\n' "$0"
    exit 1
    ;;
esac