#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$project_dir"

if ! command -v idf.py >/dev/null 2>&1; then
    if [[ -f "$HOME/.espressif/tools/activate_idf_v6.0.2.sh" ]]; then
        # shellcheck disable=SC1091
        source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
    elif [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
        # shellcheck disable=SC1090
        source "$IDF_PATH/export.sh"
    else
        echo "ESP-IDF is not active. Source its export.sh, then run this again." >&2
        exit 1
    fi
fi

port=""
monitor=false
for argument in "$@"; do
    if [[ "$argument" == "--monitor" ]]; then
        monitor=true
    elif [[ -z "$port" ]]; then
        port="$argument"
    else
        echo "Usage: ./flash.sh [PORT] [--monitor]" >&2
        exit 2
    fi
done

if [[ -z "$port" ]]; then
    shopt -s nullglob
    ports=(
        /dev/cu.usbmodem*
        /dev/cu.SLAB_USBtoUART*
        /dev/cu.wchusbserial*
        /dev/ttyACM*
        /dev/ttyUSB*
    )
    shopt -u nullglob
    if [[ ${#ports[@]} -ne 1 ]]; then
        echo "Expected one connected ESP, found ${#ports[@]}." >&2
        echo "Usage: ./flash.sh PORT [--monitor]" >&2
        printf '  %s\n' "${ports[@]}" >&2
        exit 2
    fi
    port="${ports[0]}"
fi

command=(idf.py -p "$port" flash)
if $monitor; then
    command+=(monitor)
fi

echo "Flashing Everframe on $port"
"${command[@]}"

echo "Application image: $project_dir/build/everframe-esp32.bin"
echo "Application offset: 0x10000"
