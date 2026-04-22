#!/bin/sh
set -eu

RUNTIME_BIN="${INCONSOLE_RUNTIME_BIN:-/usr/bin/inconsole-runtime}"
BRIDGE_BIN="${INCONSOLE_BRIDGE_BIN:-/usr/bin/inconsole-input-bridge}"
LOG_DIR="/userdata/system/logs"
HELPER_LOG="${INCONSOLE_LAUNCH_LOG:-${LOG_DIR}/launch-with-exit.log}"

mkdir -p "${LOG_DIR}"

ts() {
	date '+%Y-%m-%d %H:%M:%S'
}

log_line() {
	printf '%s launch-helper: %s\n' "$(ts)" "$*" >> "${HELPER_LOG}"
}

stop_pid() {
	if [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null; then
		kill "$1" 2>/dev/null || true
		sleep 1
		kill -9 "$1" 2>/dev/null || true
	fi
}

APP_PID=""
BRIDGE_PID=""
trap 'stop_pid "${BRIDGE_PID:-}"; stop_pid "${APP_PID:-}"' EXIT INT TERM

if [ "$#" -lt 1 ]; then
	log_line "missing app command"
	exec "${RUNTIME_BIN}"
fi

APP_CMD="$1"
shift

if [ ! -x "${APP_CMD}" ]; then
	log_line "app binary missing: ${APP_CMD}"
	exec "${RUNTIME_BIN}"
fi

"${APP_CMD}" "$@" &
APP_PID=$!
log_line "app started pid=${APP_PID} cmd=${APP_CMD}"

if [ -x "${BRIDGE_BIN}" ]; then
	if [ ! -e /dev/uinput ] && command -v modprobe >/dev/null 2>&1; then
		modprobe uinput >/dev/null 2>&1 || true
	fi
	if [ -e /dev/uinput ] || [ -e /dev/input/uinput ]; then
		INCONSOLE_EXIT_PID="${APP_PID}" "${BRIDGE_BIN}" >> "${HELPER_LOG}" 2>&1 &
		BRIDGE_PID=$!
		log_line "input bridge started pid=${BRIDGE_PID}"
	else
		log_line "input bridge skipped: missing uinput"
	fi
else
	log_line "input bridge missing: ${BRIDGE_BIN}"
fi

set +e
wait "${APP_PID}"
APP_RC=$?
set -e
log_line "app finished rc=${APP_RC}"

stop_pid "${BRIDGE_PID:-}"
BRIDGE_PID=""
exec "${RUNTIME_BIN}"
