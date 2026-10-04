#!/usr/bin/env bash
# Build / flash / monitor the CR-10 print server from a Linux or macOS terminal.
#
# Linux : the serial port is passed straight into the container (--device).
# macOS : Docker cannot pass USB devices; flash/monitor go through
#         esp_rfc2217_server on the host (pip install esptool).
#
#   ./idf.sh build | build-dry | menuconfig | clean | size | shell
#   ./idf.sh flash /dev/ttyUSB0 [--dry]
#   ./idf.sh monitor /dev/ttyUSB0
#   ./idf.sh flash-monitor /dev/ttyUSB0
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE=cr10-idf
CACHE=cr10-idf-cache
RFC_PORT=4000

CMD="${1:-help}"
PORT="${2:-}"
DRY=0
for a in "$@"; do [[ "$a" == "--dry" ]] && DRY=1; done
[[ "$CMD" == "build-dry" ]] && DRY=1

IDF_EXTRA=()
if [[ $DRY == 1 ]]; then
  IDF_EXTRA=(-B build_dry -D SDKCONFIG=build_dry/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.dryrun")
fi

ensure_image() {
  docker info >/dev/null 2>&1 || { echo "Docker is not running" >&2; exit 1; }
  docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$ROOT"
}

run_idf() {  # run_idf <docker flags...> -- <idf args...>
  local dflags=()
  while [[ $# -gt 0 && "$1" != "--" ]]; do dflags+=("$1"); shift; done
  shift || true
  ensure_image
  docker run --rm "${dflags[@]}" -v "$ROOT:/project" -v "$CACHE:/opt/idf-cache" -w /project \
    "$IMAGE" idf.py "${IDF_EXTRA[@]}" "$@"
}

need_port() {
  if [[ -z "$PORT" || "$PORT" == --* ]]; then
    echo "Usage: $0 $CMD <serial port>   e.g. /dev/ttyUSB0 (Linux) or /dev/cu.usbserial-* (macOS)" >&2
    ls /dev/ttyUSB* /dev/ttyACM* /dev/cu.usb* 2>/dev/null || true
    exit 1
  fi
}

serial_action() {  # serial_action <idf args...>
  need_port
  if [[ "$(uname)" == "Linux" ]]; then
    run_idf -it --device "$PORT" --group-add dialout -- -p "$PORT" "$@"
  else
    local srv
    srv="$(command -v esp_rfc2217_server || command -v esp_rfc2217_server.py || true)"
    [[ -n "$srv" ]] || { echo "Install esptool on the host: pip install esptool" >&2; exit 1; }
    "$srv" -p "$RFC_PORT" "$PORT" & local pid=$!
    trap 'kill $pid 2>/dev/null' EXIT
    sleep 1.5
    run_idf -it -- -p "rfc2217://host.docker.internal:${RFC_PORT}?ign_set_control" "$@"
  fi
}

case "$CMD" in
  build|build-dry) run_idf -- build ;;
  menuconfig)      run_idf -it -- menuconfig ;;
  clean)           run_idf -- fullclean ;;
  size)            run_idf -- size ;;
  shell)           ensure_image; docker run --rm -it -v "$ROOT:/project" -v "$CACHE:/opt/idf-cache" -w /project "$IMAGE" bash ;;
  flash)           serial_action flash ;;
  monitor)         serial_action monitor ;;
  flash-monitor)   serial_action flash monitor ;;
  erase)           serial_action erase-flash ;;
  *) sed -n '2,12p' "$0" ;;
esac
