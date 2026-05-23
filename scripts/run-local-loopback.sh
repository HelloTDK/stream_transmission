#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-$ROOT_DIR/build/weaknet_webrtc}"
CONFIG="${2:-$ROOT_DIR/config/default.yaml}"

if [[ ! -x "$BIN" ]]; then
  echo "Executable not found: $BIN" >&2
  echo "Build first: cmake --build $ROOT_DIR/build -j" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
SEND_TO_RECV="$TMP_DIR/send_to_recv"
RECV_TO_SEND="$TMP_DIR/recv_to_send"
mkfifo "$SEND_TO_RECV" "$RECV_TO_SEND"

SEND_PID=""
RECV_PID=""

cleanup() {
  if [[ -n "$SEND_PID" ]] && kill -0 "$SEND_PID" 2>/dev/null; then
    kill "$SEND_PID" 2>/dev/null || true
  fi
  if [[ -n "$RECV_PID" ]] && kill -0 "$RECV_PID" 2>/dev/null; then
    kill "$RECV_PID" 2>/dev/null || true
  fi
  wait "$SEND_PID" "$RECV_PID" 2>/dev/null || true
  exec 3>&- 4>&- || true
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

# Keep both FIFOs open in this parent process so child redirections do not
# block during startup before the opposite side has finished launching.
exec 3<>"$SEND_TO_RECV"
exec 4<>"$RECV_TO_SEND"

"$BIN" --mode send --config "$CONFIG" \
  < "$RECV_TO_SEND" \
  > "$SEND_TO_RECV" \
  2> >(sed -u 's/^/[send] /' >&2) &
SEND_PID="$!"

"$BIN" --mode recv --config "$CONFIG" \
  < "$SEND_TO_RECV" \
  > "$RECV_TO_SEND" \
  2> >(sed -u 's/^/[recv] /' >&2) &
RECV_PID="$!"

echo "Local loopback started. Press Ctrl+C to stop." >&2
wait -n "$SEND_PID" "$RECV_PID"
