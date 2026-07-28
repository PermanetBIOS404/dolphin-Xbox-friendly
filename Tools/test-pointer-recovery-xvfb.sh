#!/usr/bin/env bash

# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <PointerRecoveryNativeTest> <artifact-directory>" >&2
  exit 2
fi

test_binary="$1"
artifact_directory="$2"

if [[ ! -x "$test_binary" ]]; then
  echo "Native pointer recovery test executable is not executable: $test_binary" >&2
  exit 2
fi

for dependency in Xvfb xvfb-run xdpyinfo; do
  if ! command -v "$dependency" >/dev/null; then
    echo "Missing native pointer test dependency: $dependency" >&2
    exit 2
  fi
done

if ! pkg-config --exists xi xtst; then
  echo "Missing XInput2/XTest development packages (libxi-dev and libxtst-dev)" >&2
  exit 2
fi

qt_plugin_directory="$(qtpaths6 --plugin-dir 2>/dev/null || true)"
if [[ -z "$qt_plugin_directory" ||
      ! -f "$qt_plugin_directory/platforms/libqxcb.so" ]]; then
  echo "Qt 6 xcb platform plugin was not found under: $qt_plugin_directory" >&2
  exit 2
fi

mkdir -p "$artifact_directory"
log_path="$artifact_directory/pointer-native-x11-test.log"

echo "Native pointer recovery log: $log_path"
echo "Native pointer screenshot directory: $artifact_directory"

QT_QPA_PLATFORM=xcb \
QT_XCB_NO_XI2=1 \
LIBGL_ALWAYS_SOFTWARE=1 \
DOLPHIN_POINTER_ARTIFACT_DIR="$artifact_directory" \
  xvfb-run -a -s "-screen 0 1280x720x24 -nolisten tcp" \
  sh -c '
    set -eu
    extensions="$(xdpyinfo -queryExtensions)"
    printf "%s\n" "$extensions" | grep -q "XInputExtension"
    printf "%s\n" "$extensions" | grep -q "XTEST"
    exec "$1"
  ' sh "$test_binary" 2>&1 | tee "$log_path"
