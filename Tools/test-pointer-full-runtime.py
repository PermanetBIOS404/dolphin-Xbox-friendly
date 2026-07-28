#!/usr/bin/env python3

# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Disposable-profile X11 automation for Dolphin's full Wii pointer runtime."""

from __future__ import annotations

import argparse
import configparser
import ctypes
import ctypes.util
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
COPIED_CONFIGS = ("Dolphin.ini", "GFX.ini", "Logger.ini", "WiimoteNew.ini")
A_EXPRESSION = "(`Xbox/0/Controller:A`) | `Click 1`"
B_EXPRESSION = "(`Xbox/0/Controller:Trigger R`) | `Click 3`"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_files(source: Path) -> list[Path]:
    files = [path for path in (source / "Wii").rglob("*") if path.is_file()]
    files.extend(source / "Config" / name for name in COPIED_CONFIGS)
    return sorted(files)


def source_integrity(source: Path) -> list[str]:
    result = []
    for path in source_files(source):
        stat = path.stat()
        result.append(
            f"{sha256(path)} {stat.st_size} {stat.st_mtime_ns} "
            f"{path.relative_to(source)}"
        )
    return result


def read_ini(path: Path) -> configparser.ConfigParser:
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    if path.exists():
        config.read(path, encoding="utf-8")
    return config


def write_ini(config: configparser.ConfigParser, path: Path) -> None:
    with path.open("w", encoding="utf-8") as stream:
        config.write(stream, space_around_delimiters=True)


def configure_disposable_profile(profile: Path) -> None:
    config_dir = profile / "Config"
    dolphin_path = config_dir / "Dolphin.ini"
    dolphin = read_ini(dolphin_path)
    for section in ("Analytics", "General", "Core"):
        if not dolphin.has_section(section):
            dolphin.add_section(section)
    dolphin["Analytics"]["PermissionAsked"] = "True"
    dolphin["General"]["HotkeysRequireFocus"] = "True"
    dolphin["General"]["WiiSDCardPath"] = str(profile / "Load" / "WiiSD.raw")
    for key in ("WiiSDCardPhysicalPath", "WiiSDCardSyncFolder"):
        dolphin["General"].pop(key, None)
    dolphin["Core"].update(
        {
            "WiiSDCard": "True",
            "WiiSDCardAllowWrites": "False",
            "WiiSDCardEnableFolderSync": "False",
            "WiiSDCardFilesize": "134217728",
            "WiiSDCardUsePhysical": "False",
            "WiiSDStorageMode": "0",
            "WiimoteControllerInterface": "True",
            "WiimoteSource0": "1",
        }
    )
    dolphin["Core"].pop("WiiSDPhysicalDevicePath", None)
    write_ini(dolphin, dolphin_path)

    wiimote = configparser.ConfigParser(interpolation=None)
    wiimote.optionxform = str
    wiimote["Wiimote1"] = {
        "Buttons/A": A_EXPRESSION,
        "Buttons/B": B_EXPRESSION,
        "Buttons/1": "`1`",
        "Buttons/2": "`2`",
        "Buttons/-": "`Q`",
        "Buttons/+": "`E`",
        "Buttons/Home": "`Return`",
        "D-Pad/Up": "`Up`",
        "D-Pad/Down": "`Down`",
        "D-Pad/Left": "`Left`",
        "D-Pad/Right": "`Right`",
        "IR/Up": "`Cursor Y-`",
        "IR/Down": "`Cursor Y+`",
        "IR/Left": "`Cursor X-`",
        "IR/Right": "`Cursor X+`",
        "IR/Relative Input": "False",
        "Device": "XInput2/0/Virtual core pointer",
        "Source": "1",
    }
    for index in range(2, 5):
        wiimote[f"Wiimote{index}"] = {
            "Device": "XInput2/0/Virtual core pointer",
            "Source": "0",
        }
    wiimote["BalanceBoard"] = {
        "Device": "XInput2/0/Virtual core pointer",
        "Source": "0",
    }
    write_ini(wiimote, config_dir / "WiimoteNew.ini")

    hotkeys = configparser.ConfigParser(interpolation=None)
    hotkeys.optionxform = str
    hotkeys["Hotkeys"] = {
        "Device": "XInput2/0/Virtual core pointer",
        "General/Open Dolphin Quick Menu": "@(Ctrl+Shift+Space)",
    }
    write_ini(hotkeys, config_dir / "Hotkeys.ini")


def prepare_profile(source: Path, profile: Path, artifacts: Path) -> None:
    if any(profile.iterdir()):
        raise RuntimeError(f"Disposable profile is not empty: {profile}")

    before = source_integrity(source)
    integrity_path = artifacts / "disposable-profile-source-integrity.txt"
    integrity_path.write_text("\n".join(before) + "\n", encoding="utf-8")

    shutil.copytree(source / "Wii", profile / "Wii")
    (profile / "Config").mkdir()
    for name in COPIED_CONFIGS:
        shutil.copy2(source / "Config" / name, profile / "Config" / name)
    for name in ("Cache", "Load", "Logs", "ScreenShots"):
        (profile / name).mkdir()
    configure_disposable_profile(profile)

    copied = sorted(path for path in profile.rglob("*") if path.is_file())
    total_size = sum(path.stat().st_size for path in copied)
    manifest = [
        f"profile={profile}",
        f"source={source}",
        f"file_count={len(copied)}",
        f"total_size_bytes={total_size}",
        "excluded=Backup Cache Dump GBA GC GameSettings Installers Load/WiiSD.raw "
        "Logs Maps ResourcePacks SavedAssembly ScreenShots Shaders StateSaves Styles Themes "
        "Triforce WFS",
    ]
    manifest.extend(
        f"{path.stat().st_size} {path.relative_to(profile)}" for path in copied
    )
    (artifacts / "disposable-profile-manifest.txt").write_text(
        "\n".join(manifest) + "\n", encoding="utf-8"
    )


def verify_source_integrity(source: Path, artifacts: Path) -> None:
    expected = (
        artifacts / "disposable-profile-source-integrity.txt"
    ).read_text(encoding="utf-8").splitlines()
    actual = source_integrity(source)
    if actual != expected:
        raise RuntimeError("Original Virtual Wii source changed during the test")


def verify_disposable_bindings(profile: Path) -> None:
    wiimote = read_ini(profile / "Config" / "WiimoteNew.ini")
    if wiimote["Wiimote1"].get("Buttons/A") != A_EXPRESSION:
        raise RuntimeError("Disposable Wii A expression changed during the test")
    if wiimote["Wiimote1"].get("Buttons/B") != B_EXPRESSION:
        raise RuntimeError("Disposable Wii B expression changed during the test")


def wmctrl_windows() -> list[tuple[str, int, str]]:
    result = subprocess.run(
        ["wmctrl", "-lp"], check=True, capture_output=True, text=True
    )
    windows = []
    for line in result.stdout.splitlines():
        fields = line.split(maxsplit=4)
        if len(fields) >= 4:
            try:
                windows.append((fields[0], int(fields[2]), fields[4] if len(fields) > 4 else ""))
            except ValueError:
                pass
    return windows


def wait_for_main_window(process: subprocess.Popen) -> str:
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Dolphin exited early with status {process.returncode}")
        for xid, pid, _ in wmctrl_windows():
            if pid == process.pid:
                return xid
        time.sleep(0.25)
    raise RuntimeError("Dolphin did not expose an X11 top-level window")


def telemetry_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return ""


def wait_telemetry(path: Path, pattern: str, timeout: float = 30, count: int = 1) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = telemetry_text(path)
        if len(re.findall(pattern, text)) >= count:
            return text
        time.sleep(0.2)
    raise RuntimeError(f"Telemetry timeout waiting for {pattern!r}")


class XTest:
    def __init__(self):
        self.x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        self.xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.display = self.x11.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("XOpenDisplay failed")
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.xtst.XTestFakeMotionEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_ulong,
        ]
        self.xtst.XTestFakeMotionEvent.restype = ctypes.c_int
        self.xtst.XTestFakeKeyEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_int,
            ctypes.c_ulong,
        ]
        self.xtst.XTestFakeKeyEvent.restype = ctypes.c_int
        self.xtst.XTestFakeButtonEvent.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint,
            ctypes.c_int,
            ctypes.c_ulong,
        ]
        self.xtst.XTestFakeButtonEvent.restype = ctypes.c_int
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XKeysymToKeycode.restype = ctypes.c_uint
        self.x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
        self.x11.XStringToKeysym.restype = ctypes.c_ulong

    def motion(self, x: int, y: int) -> None:
        if not self.xtst.XTestFakeMotionEvent(self.display, 0, x, y, 0):
            raise RuntimeError("XTestFakeMotionEvent failed")
        self.x11.XFlush(self.display)

    def chord(self, names: tuple[str, ...]) -> None:
        keycodes = []
        for name in names:
            keysym = self.x11.XStringToKeysym(name.encode())
            keycodes.append(self.x11.XKeysymToKeycode(self.display, keysym))
        for keycode in keycodes:
            self.xtst.XTestFakeKeyEvent(self.display, keycode, True, 0)
        self.x11.XFlush(self.display)
        time.sleep(0.1)
        for keycode in reversed(keycodes):
            self.xtst.XTestFakeKeyEvent(self.display, keycode, False, 0)
        self.x11.XFlush(self.display)

    def press(self, name: str) -> None:
        keysym = self.x11.XStringToKeysym(name.encode())
        keycode = self.x11.XKeysymToKeycode(self.display, keysym)
        self.xtst.XTestFakeKeyEvent(self.display, keycode, True, 0)
        self.xtst.XTestFakeKeyEvent(self.display, keycode, False, 0)
        self.x11.XFlush(self.display)

    def click(self, x: int, y: int) -> None:
        self.motion(x, y)
        self.xtst.XTestFakeButtonEvent(self.display, 1, True, 0)
        self.xtst.XTestFakeButtonEvent(self.display, 1, False, 0)
        self.x11.XFlush(self.display)


def open_quick_menu_through_emulation_menu(
    xtest: XTest, main_window: str
) -> None:
    subprocess.run(["wmctrl", "-ia", main_window], check=True)
    time.sleep(0.25)
    xtest.chord(("Alt_L", "e"))
    time.sleep(0.15)
    # The Quick Menu action has a stable D mnemonic in the Emulation menu.
    xtest.press("d")


def xid_from_telemetry(text: str, key: str) -> str:
    matches = re.findall(rf"{key}=(0x[0-9a-fA-F]+|[0-9]+)", text)
    if not matches:
        raise RuntimeError(f"No {key} found in telemetry")
    return matches[-1]


def x11_xid_arg(xid: str) -> str:
    return hex(int(xid, 0))


def geometry_from_telemetry(text: str, key: str) -> tuple[int, int, int, int]:
    matches = re.findall(rf"{key}=(-?\d+),(-?\d+),(\d+)x(\d+)", text)
    if not matches:
        raise RuntimeError(f"No {key} found in telemetry")
    return tuple(int(value) for value in matches[-1])


def capture_window(xid: str, path: Path) -> None:
    subprocess.run(
        ["import", "-window", x11_xid_arg(xid), str(path)], check=True, timeout=15
    )


def record_x11_windows(path: Path, label: str) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(f"\n===== {label} =====\n")
        subprocess.run(["xwininfo", "-root", "-tree"], stdout=stream, stderr=subprocess.STDOUT,
                       check=True)


def run(args) -> None:
    focus_phase = args.phase.startswith("focus-")
    phase_label = args.phase.removeprefix("focus-")
    artifact_prefix = "quick-menu-focus" if focus_phase else "pointer-full-runtime"
    telemetry = args.artifacts / f"{artifact_prefix}-{phase_label}.log"
    telemetry.unlink(missing_ok=True)
    output_path = args.artifacts / (
        "quick-menu-focus-automation.log" if focus_phase else "automation-output.log"
    )
    window_tree_path = args.artifacts / (
        "quick-menu-focus-window-tree.txt" if focus_phase else "full-runtime-window-tree.txt"
    )
    screenshot_path = args.artifacts / f"{artifact_prefix}-{phase_label}.png"

    environment = os.environ.copy()
    environment.update(
        {
            "DOLPHIN_POINTER_E2E_LOG": str(telemetry),
            "DOLPHIN_POINTER_E2E_AUTO_BOOT": "1",
            "DOLPHIN_POINTER_E2E_AUTO_QUICK_MENU": "1",
            "QT_QPA_PLATFORM": "xcb",
            "LIBGL_ALWAYS_SOFTWARE": "1",
        }
    )
    with output_path.open("a", encoding="utf-8") as output:
        output.write(f"\n===== phase={args.phase} profile={args.profile} =====\n")
        output.flush()
        process = subprocess.Popen(
            [str(args.executable), "-u", str(args.profile)],
            stdout=output,
            stderr=subprocess.STDOUT,
            env=environment,
        )
        try:
            main_window = wait_for_main_window(process)
            subprocess.run(["wmctrl", "-ia", main_window], check=True)
            xtest = XTest()
            xtest.motion(640, 360)

            wait_telemetry(telemetry, r"event=system_menu_action_triggered", timeout=10)
            text = wait_telemetry(telemetry, r"event=core_running", timeout=45)
            render_xid = xid_from_telemetry(text, "render_xid")
            xtest.motion(640, 360)
            time.sleep(3)
            capture_window(render_xid, screenshot_path)
            record_x11_windows(window_tree_path, f"{args.phase}: core running")

            wait_telemetry(telemetry, r"event=quick_menu_action_automation", timeout=10)
            text = wait_telemetry(telemetry, r"event=quick_menu_post_event.*visible=true",
                                  timeout=12)
            quick_menu_xid = xid_from_telemetry(text, "xid")
            subprocess.run(["xwininfo", "-id", x11_xid_arg(quick_menu_xid)], check=True,
                           stdout=output, stderr=subprocess.STDOUT)
            capture_window(quick_menu_xid, args.artifacts / "quick-menu-full-runtime.png")
            resume_x, resume_y, resume_width, resume_height = geometry_from_telemetry(
                text, "resume_geometry"
            )
            render_x, render_y, render_width, render_height = geometry_from_telemetry(
                text, "geometry"
            )
            if resume_width <= 0 or resume_height <= 0:
                raise RuntimeError("Quick Menu Resume button has empty geometry")
            valid_recovery_pattern = (
                r"event=wiimote_ir_sample .*recovery_executed=true "
                r".*point_hidden=false .*ir_valid=true"
            )
            valid_recoveries = len(
                re.findall(valid_recovery_pattern, telemetry_text(telemetry))
            )
            xtest.click(resume_x + resume_width // 2, resume_y + resume_height // 2)
            time.sleep(0.2)
            if "event=quick_menu_closed" not in telemetry_text(telemetry):
                xtest.click(resume_x + resume_width // 2, resume_y + resume_height // 2)
            time.sleep(0.5)
            if args.phase == "focus-before-fix":
                capture_window(render_xid, screenshot_path)
                record_x11_windows(window_tree_path, f"{args.phase}: after first close")

            if args.phase == "focus-before-fix":
                return

            if focus_phase:
                wait_telemetry(telemetry, r"event=quick_menu_focus_restored", timeout=10)
                wait_telemetry(
                    telemetry,
                    valid_recovery_pattern,
                    timeout=10,
                    count=valid_recoveries + 1,
                )

            quick_menu_requests = len(
                re.findall(r"event=quick_menu_mainwindow_request", telemetry_text(telemetry))
            )
            # Exercise the configured shortcut first. Some host environments reserve this
            # combination; that outcome is recorded explicitly and the second lifecycle cycle
            # still proceeds through Dolphin's real Emulation menu action.
            time.sleep(0.5)
            xtest.chord(("Control_L", "Shift_L", "space"))
            try:
                text = wait_telemetry(
                    telemetry,
                    r"event=quick_menu_mainwindow_request",
                    timeout=2,
                    count=quick_menu_requests + 1,
                )
                output.write("quick-menu hotkey delivered with HotkeysRequireFocus=True\n")
            except RuntimeError:
                output.write(
                    "quick-menu hotkey was not delivered; opening through the Emulation menu\n"
                )
                output.flush()
                open_quick_menu_through_emulation_menu(xtest, main_window)
                text = wait_telemetry(
                    telemetry,
                    r"event=quick_menu_mainwindow_request",
                    timeout=10,
                    count=quick_menu_requests + 1,
                )
            text = wait_telemetry(
                telemetry,
                r"event=quick_menu_post_event.*visible=true",
                timeout=10,
                count=2,
            )
            resume_x, resume_y, resume_width, resume_height = geometry_from_telemetry(
                text, "resume_geometry"
            )
            quick_menu_xid = xid_from_telemetry(text, "xid")
            xtest.click(resume_x + resume_width // 2, resume_y + resume_height // 2)
            time.sleep(0.2)
            if len(
                re.findall(r"event=quick_menu_closed", telemetry_text(telemetry))
            ) < 2:
                xtest.click(resume_x + resume_width // 2, resume_y + resume_height // 2)
            if focus_phase:
                wait_telemetry(
                    telemetry, r"event=quick_menu_focus_restored", timeout=10, count=2
                )
                wait_telemetry(
                    telemetry,
                    valid_recovery_pattern,
                    timeout=10,
                    count=valid_recoveries + 2,
                )

                # Exercise both recovery buttons through the actual top-level Quick Menu.
                for cycle, geometry_key in (
                    (3, "restore_geometry"),
                    (4, "reconnect_geometry"),
                ):
                    requests_before = len(
                        re.findall(
                            r"event=quick_menu_mainwindow_request",
                            telemetry_text(telemetry),
                        )
                    )
                    backends_before = len(
                        re.findall(
                            r"event=xinput2_backend_created", telemetry_text(telemetry)
                        )
                    )
                    open_quick_menu_through_emulation_menu(xtest, main_window)
                    wait_telemetry(
                        telemetry,
                        r"event=quick_menu_mainwindow_request",
                        timeout=10,
                        count=requests_before + 1,
                    )
                    text = wait_telemetry(
                        telemetry,
                        r"event=quick_menu_post_event.*visible=true",
                        timeout=10,
                        count=cycle,
                    )
                    button_x, button_y, button_width, button_height = (
                        geometry_from_telemetry(text, geometry_key)
                    )
                    if button_width <= 0 or button_height <= 0:
                        raise RuntimeError(
                            f"Quick Menu {geometry_key} button has empty geometry"
                        )
                    xtest.click(
                        button_x + button_width // 2,
                        button_y + button_height // 2,
                    )
                    wait_telemetry(
                        telemetry,
                        r"event=quick_menu_closed",
                        timeout=10,
                        count=cycle,
                    )
                    wait_telemetry(
                        telemetry,
                        r"event=quick_menu_focus_restored",
                        timeout=10,
                        count=cycle,
                    )
                    wait_telemetry(
                        telemetry,
                        valid_recovery_pattern,
                        timeout=10,
                        count=valid_recoveries + cycle,
                    )
                    if geometry_key == "reconnect_geometry":
                        wait_telemetry(
                            telemetry,
                            r"event=xinput2_backend_created",
                            timeout=10,
                            count=backends_before + 1,
                        )

                capture_window(render_xid, screenshot_path)
            record_x11_windows(window_tree_path, f"{args.phase}: after hotkey")

            if args.phase in ("after-fix", "focus-after-fix"):
                text = telemetry_text(telemetry)
                if "event=initial_activation_complete" not in text:
                    raise RuntimeError("Initial pointer activation never reached completion")
                if not re.search(
                    r"event=wiimote_ir_sample .*point_hidden=false .*ir_valid=true", text
                ):
                    raise RuntimeError("No finite, visible Point with valid emulated IR was observed")
        finally:
            if process.poll() is None:
                text = telemetry_text(telemetry)
                try:
                    main_xid = xid_from_telemetry(text, "main_xid")
                    subprocess.run(["wmctrl", "-ic", hex(int(main_xid, 0))], check=False)
                    time.sleep(1)
                    xtest.chord(("Alt_L", "y"))
                except Exception as error:
                    output.write(f"clean-close warning: {error}\n")
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=5)
        if process.returncode not in (0, None):
            raise RuntimeError(f"Dolphin exited with status {process.returncode}")


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument(
        "--phase",
        choices=("before-fix", "after-fix", "focus-before-fix", "focus-after-fix"),
    )
    parser.add_argument("--prepare-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    args.profile.mkdir(parents=True, exist_ok=True)
    marker = args.profile / ".pointer-e2e-profile"
    if not marker.exists():
        prepare_profile(args.source, args.profile, args.artifacts)
        marker.write_text("disposable Dolphin pointer E2E profile\n", encoding="utf-8")
    if not args.prepare_only:
        if args.executable is None or args.phase is None:
            raise RuntimeError("--executable and --phase are required for a run")
        configure_disposable_profile(args.profile)
        try:
            run(args)
        finally:
            verify_source_integrity(args.source, args.artifacts)
            verify_disposable_bindings(args.profile)
    else:
        verify_source_integrity(args.source, args.artifacts)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"pointer full-runtime automation failed: {error}", file=sys.stderr)
        raise
