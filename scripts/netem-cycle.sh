#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-wlan0}"
INTERVAL_SECONDS="${2:-10}"
REPEAT="${3:-0}"

STAGES=(
  "good|0%|0ms|0ms"
  "medium|5%|50ms|10ms"
  "bad|20%|100ms|20ms"
  "severe|45%|150ms|40ms"
  "extreme|80%|200ms|50ms"
)

usage() {
  cat <<EOF
Usage:
  sudo $0 [iface] [interval_seconds] [repeat]

Examples:
  sudo $0 wlan0
  sudo $0 wlan0 10
  sudo $0 wlan0 10 1

Arguments:
  iface             Network interface to shape. Default: wlan0
  interval_seconds  Seconds to keep each stage. Default: 10
  repeat            Number of full cycles. 0 means loop forever. Default: 0

The script clears tc qdisc on exit.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "$(id -u)" -ne 0 ]]; then
  echo "This script must run as root because tc qdisc requires CAP_NET_ADMIN." >&2
  echo "Try: sudo $0 $IFACE $INTERVAL_SECONDS $REPEAT" >&2
  exit 1
fi

if ! command -v tc >/dev/null 2>&1; then
  echo "tc not found. Install iproute2 first." >&2
  exit 1
fi

if [[ ! -d "/sys/class/net/$IFACE" ]]; then
  echo "Network interface not found: $IFACE" >&2
  echo "Available interfaces:" >&2
  ls /sys/class/net >&2
  exit 1
fi

cleanup() {
  tc qdisc del dev "$IFACE" root 2>/dev/null || true
  echo "netem cleared on $IFACE" >&2
}
trap cleanup EXIT INT TERM

apply_stage() {
  local name="$1"
  local loss="$2"
  local delay="$3"
  local jitter="$4"

  if [[ "$loss" == "0%" && "$delay" == "0ms" ]]; then
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    echo "[$(date '+%H:%M:%S')] stage=$name clear netem; sleep ${INTERVAL_SECONDS}s"
  else
    tc qdisc replace dev "$IFACE" root netem loss "$loss" delay "$delay" "$jitter"
    echo "[$(date '+%H:%M:%S')] stage=$name loss=$loss delay=$delay jitter=$jitter; sleep ${INTERVAL_SECONDS}s"
  fi
}

cycle=0
while [[ "$REPEAT" == "0" || "$cycle" -lt "$REPEAT" ]]; do
  cycle=$((cycle + 1))
  echo "Starting netem cycle $cycle on $IFACE"

  for stage in "${STAGES[@]}"; do
    IFS="|" read -r name loss delay jitter <<< "$stage"
    apply_stage "$name" "$loss" "$delay" "$jitter"
    sleep "$INTERVAL_SECONDS"
  done
done
