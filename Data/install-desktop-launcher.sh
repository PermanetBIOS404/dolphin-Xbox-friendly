#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
dolphin_executable="${script_dir}/dolphin-emu"
launcher_icon="${script_dir}/dolphin-rwin.png"

if [[ ! -f "${dolphin_executable}" ]]; then
  printf 'Error: expected dolphin-emu at %s\n' "${dolphin_executable}" >&2
  exit 1
fi

if [[ ! -x "${dolphin_executable}" ]]; then
  printf 'Error: dolphin-emu is not executable: %s\n' "${dolphin_executable}" >&2
  exit 1
fi

if [[ ! -f "${launcher_icon}" ]]; then
  printf 'Error: expected launcher artwork at %s\n' "${launcher_icon}" >&2
  exit 1
fi

if [[ "${script_dir}" == *$'\n'* || "${script_dir}" == *$'\r'* ]]; then
  printf 'Error: the Dolphin RWiN directory path cannot contain a line break.\n' >&2
  exit 1
fi

# Desktop Entry Exec values use double quotes, with these characters escaped.
desktop_exec="${dolphin_executable//\\/\\\\\\\\}"
desktop_exec="${desktop_exec//\"/\\\"}"
desktop_exec="${desktop_exec//\$/\\\$}"
desktop_exec="${desktop_exec//\`/\\\`}"

# Backslashes must also be escaped in ordinary Desktop Entry string values.
desktop_icon="${launcher_icon//\\/\\\\}"

applications_dir="${HOME:?HOME is not set}/.local/share/applications"
desktop_file="${applications_dir}/dolphin-rwin.desktop"
mkdir -p -- "${applications_dir}"

temporary_file="$(mktemp "${applications_dir}/.dolphin-rwin.desktop.XXXXXX")"
cleanup() {
  rm -f -- "${temporary_file}"
}
trap cleanup EXIT

{
  printf '%s\n' '[Desktop Entry]'
  printf '%s\n' 'Version=1.0'
  printf '%s\n' 'Name=Dolphin RWiN'
  printf '%s\n' 'GenericName=Wii/GameCube Emulator'
  printf '%s\n' 'Comment=Launch Dolphin RWiN'
  printf 'Exec="%s"\n' "${desktop_exec}"
  printf 'Icon=%s\n' "${desktop_icon}"
  printf '%s\n' 'Type=Application'
  printf '%s\n' 'Terminal=false'
  printf '%s\n' 'Categories=Game;Emulator;'
} > "${temporary_file}"

chmod 0644 "${temporary_file}"
mv -f -- "${temporary_file}" "${desktop_file}"
trap - EXIT

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "${applications_dir}" >/dev/null 2>&1 || true
fi

printf 'Dolphin RWiN launcher installed at %s\n' "${desktop_file}"
