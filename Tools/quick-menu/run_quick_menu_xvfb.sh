#!/usr/bin/env bash

# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

usage() {
  echo "Usage: $0 qt-test <dolphin-emu> [artifact-dir]" >&2
  echo "       $0 dogtail <dolphin-emu> <artifact-dir>" >&2
}

if [[ $# -lt 2 ]]; then
  usage
  exit 2
fi

mode="$1"
dolphin="$2"
artifacts="${3:-}"

if [[ ! -x "$dolphin" ]]; then
  echo "Dolphin executable is not executable: $dolphin" >&2
  exit 2
fi

test_root="$(mktemp -d -t dolphin-quick-menu-xvfb-XXXXXX)"
cleanup() {
  if [[ -n "$test_root" && -d "$test_root" ]]; then
    rm -rf -- "$test_root"
  fi
}
trap cleanup EXIT

case "$mode" in
qt-test)
  mkdir -p "$test_root/user"
  DOLPHIN_QUICK_MENU_QT_TEST=1 \
    dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24" \
    "$dolphin" -u "$test_root/user"
  ;;
dogtail)
  if [[ -z "$artifacts" ]]; then
    usage
    exit 2
  fi
  mkdir -p "$artifacts"
  dbus-run-session -- xvfb-run -a -s "-screen 0 1280x800x24" \
    python3 "$script_dir/quick_menu_dogtail_smoke.py" \
    --dolphin "$dolphin" --artifacts "$artifacts"
  ;;
*)
  usage
  exit 2
  ;;
esac
