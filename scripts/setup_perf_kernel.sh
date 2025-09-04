#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# setup_perf_kernel.sh
# Helper to configure kernel parameters required for using 'perf' (and similar profiling tools) inside this environment.
# Provides apply / restore / status / persist actions.
# Usage:
#   sudo ./scripts/setup_perf_kernel.sh apply            # apply relaxed settings (non-persistent)
#   sudo ./scripts/setup_perf_kernel.sh persist          # write /etc/sysctl.d/99-t2d-perf.conf and reload
#   sudo ./scripts/setup_perf_kernel.sh restore          # restore previously saved values (only if backup exists)
#   sudo ./scripts/setup_perf_kernel.sh status           # show current values
#   sudo ./scripts/setup_perf_kernel.sh help             # print help
#
# What we change (defaults vary by distro):
#   kernel.perf_event_paranoid : lower (0 or -1) to allow user profiling incl. kernel stacks
#   kernel.kptr_restrict       : 0 to allow resolving kernel symbols
#   kernel.perf_event_max_stack: increase (e.g. 512) for deeper call stacks
#   kernel.nmi_watchdog        : optional disable (1->0) to reduce minor overhead (leave off by default)
#
# Safety:
#   - We capture original values once (on first apply) into BACKUP_FILE.
#   - 'restore' replays them if present.
#   - Persistent config placed in /etc/sysctl.d/99-t2d-perf.conf (commented header + values).
#
# NOTE: Run as root. Script avoids silent failures.

set -euo pipefail

REQUIRED_PARAMS=(
	"kernel.perf_event_paranoid"
	"kernel.kptr_restrict"
	"kernel.perf_event_max_stack"
)
# Optional parameter (disabled by default) - enable via ENABLE_NMI_WATCHDOG=0 environment variable if desired.
OPTIONAL_PARAM="kernel.nmi_watchdog"

# Desired values (can override via env):
PERF_EVENT_PARANOID_TARGET=${PERF_EVENT_PARANOID_TARGET:=-1} # -1: most permissive
KPTR_RESTRICT_TARGET=${KPTR_RESTRICT_TARGET:=0}
PERF_EVENT_MAX_STACK_TARGET=${PERF_EVENT_MAX_STACK_TARGET:=512}
# Only applied if ENABLE_NMI_WATCHDOG is set (any value) to 0 or 1 override
NMI_WATCHDOG_TARGET=${NMI_WATCHDOG_TARGET:=0}

BACKUP_FILE="/var/tmp/t2d_perf_sysctl_backup"
PERSIST_FILE="/etc/sysctl.d/99-t2d-perf.conf"

need_root() {
	if [[ $(id -u) -ne 0 ]]; then
		echo "[error] Must run as root (sudo)" >&2
		exit 1
	fi
}

print_status() {
	echo "Current kernel profiling related sysctls:"
	for p in "${REQUIRED_PARAMS[@]}" "$OPTIONAL_PARAM"; do
		if sysctl -a 2>/dev/null | grep -q "^$p ="; then
			sysctl -n "$p" 1>/dev/null 2>&1 || true
			val=$(sysctl "$p" 2>/dev/null | awk -F' = ' '{print $2}')
			echo "  $p = $val"
		fi
	done
	if [[ -f $BACKUP_FILE ]]; then
		echo "Backup file exists: $BACKUP_FILE"
	else
		echo "No backup file yet (apply not performed)"
	fi
}

backup_originals() {
	if [[ -f $BACKUP_FILE ]]; then
		echo "[info] Backup already exists: $BACKUP_FILE (won't overwrite)" >&2
		return
	fi
	echo "# Original sysctl values captured $(date -u)" >"$BACKUP_FILE".tmp
	for p in "${REQUIRED_PARAMS[@]}" "$OPTIONAL_PARAM"; do
		if sysctl -a 2>/dev/null | grep -q "^$p ="; then
			val=$(sysctl "$p" | awk -F' = ' '{print $2}')
			echo "$p=$val" >>"$BACKUP_FILE".tmp
		fi
	done
	mv "$BACKUP_FILE".tmp "$BACKUP_FILE"
	chmod 600 "$BACKUP_FILE"
	echo "[info] Original values saved to $BACKUP_FILE"
}

apply_runtime() {
	need_root
	backup_originals
	echo "[info] Applying runtime sysctl tweaks (non-persistent)" >&2
	sysctl -w kernel.perf_event_paranoid="$PERF_EVENT_PARANOID_TARGET"
	sysctl -w kernel.kptr_restrict="$KPTR_RESTRICT_TARGET"
	sysctl -w kernel.perf_event_max_stack="$PERF_EVENT_MAX_STACK_TARGET" || true
	if [[ ${ENABLE_NMI_WATCHDOG:-} != "" ]]; then
		sysctl -w kernel.nmi_watchdog="$NMI_WATCHDOG_TARGET" || true
	fi
	echo "[info] Done. (Use 'status' to verify)" >&2
}

write_persist() {
	need_root
	backup_originals
	echo "[info] Writing persistent sysctl file: $PERSIST_FILE" >&2
	cat >"$PERSIST_FILE" <<EOF
# 99-t2d-perf.conf
# Added by setup_perf_kernel.sh (t2d) on $(date -u)
# Adjusts kernel settings for user-space profiling with 'perf'.
# Original values backed up in $BACKUP_FILE
kernel.perf_event_paranoid = $PERF_EVENT_PARANOID_TARGET
kernel.kptr_restrict = $KPTR_RESTRICT_TARGET
kernel.perf_event_max_stack = $PERF_EVENT_MAX_STACK_TARGET
# Uncomment to disable watchdog (slight overhead reduction)
# kernel.nmi_watchdog = 0
EOF
	chmod 644 "$PERSIST_FILE"
	sysctl --system >/dev/null 2>&1 || sysctl -p "$PERSIST_FILE" || true
	echo "[info] Persistent config applied." >&2
}

restore_values() {
	need_root
	if [[ ! -f $BACKUP_FILE ]]; then
		echo "[error] No backup file to restore: $BACKUP_FILE" >&2
		exit 1
	fi
	echo "[info] Restoring values from $BACKUP_FILE" >&2
	while IFS='=' read -r key val; do
		[[ -z "$key" || "$key" =~ ^# ]] && continue
		if sysctl -a 2>/dev/null | grep -q "^$key ="; then
			sysctl -w "$key"="$val" || true
		fi
	done <"$BACKUP_FILE"
	echo "[info] Restore complete." >&2
}

usage() {
	grep '^# ' "$0" | sed 's/^# //'
}

cmd=${1:-help}
case $cmd in
apply) apply_runtime ;;
persist) write_persist ;;
restore) restore_values ;;
status) print_status ;;
help | *) usage ;;
esac
