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
POINT_EXPRESSIONS = ("`Cursor Y-`", "`Cursor Y+`", "`Cursor X-`", "`Cursor X+`")
TMP_ROOT = Path("/tmp")
FORBIDDEN_REAL_PROFILE = Path(
    "/media/angelo/Jane404-Ext/Emulation/saves/VirtualWii"
)


def validate_automation_paths(source: Path, profile: Path, artifacts: Path) -> None:
    source_path = source.resolve()
    profile_path = profile.resolve()
    artifacts_path = artifacts.resolve()
    forbidden_path = FORBIDDEN_REAL_PROFILE
    tmp_path = TMP_ROOT

    if source_path == forbidden_path or forbidden_path in source_path.parents:
        raise RuntimeError(f"Refusing to access the real Virtual Wii profile: {source_path}")
    for label, path in (
        ("source", source_path),
        ("profile", profile_path),
        ("artifacts", artifacts_path),
    ):
        if path != tmp_path and tmp_path not in path.parents:
            raise RuntimeError(f"Disposable {label} must be under /tmp: {path}")
    if source_path == profile_path or source_path in profile_path.parents:
        raise RuntimeError("Disposable profile must not overlap its source")


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

    # The host-side Quick Menu QAction does not use the controller HotkeyManager profile.
    (config_dir / "Hotkeys.ini").unlink(missing_ok=True)


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
    for key, expression in zip(("IR/Up", "IR/Down", "IR/Left", "IR/Right"),
                               POINT_EXPRESSIONS):
        if wiimote["Wiimote1"].get(key) != expression:
            raise RuntimeError(f"Disposable Wii {key} expression changed during the test")
    hotkey_path = profile / "Config" / "Hotkeys.ini"
    if hotkey_path.exists() and "Open Dolphin Quick Menu" in hotkey_path.read_text(
        encoding="utf-8"
    ):
        raise RuntimeError("Disposable profile unexpectedly contains a Quick Menu hotkey")


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


def telemetry_count(path: Path, pattern: str) -> int:
    return len(re.findall(pattern, telemetry_text(path)))


class XTest:
    def __init__(self):
        self.x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
        self.xtst = ctypes.CDLL(ctypes.util.find_library("Xtst"))
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.display = self.x11.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("XOpenDisplay failed")
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
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
            keycode = self.x11.XKeysymToKeycode(self.display, keysym)
            if not keycode:
                raise RuntimeError(f"X11 has no keycode for {name}")
            keycodes.append(keycode)
        for keycode in keycodes:
            if not self.xtst.XTestFakeKeyEvent(self.display, keycode, True, 0):
                raise RuntimeError(f"XTestFakeKeyEvent press failed for keycode {keycode}")
            self.x11.XSync(self.display, False)
            time.sleep(0.03)
        time.sleep(0.1)
        for keycode in reversed(keycodes):
            if not self.xtst.XTestFakeKeyEvent(self.display, keycode, False, 0):
                raise RuntimeError(f"XTestFakeKeyEvent release failed for keycode {keycode}")
            self.x11.XSync(self.display, False)
            time.sleep(0.03)

    def press(self, name: str) -> None:
        keysym = self.x11.XStringToKeysym(name.encode())
        keycode = self.x11.XKeysymToKeycode(self.display, keysym)
        if not keycode:
            raise RuntimeError(f"X11 has no keycode for {name}")
        if not self.xtst.XTestFakeKeyEvent(self.display, keycode, True, 0):
            raise RuntimeError(f"XTestFakeKeyEvent press failed for keycode {keycode}")
        self.x11.XSync(self.display, False)
        time.sleep(0.03)
        if not self.xtst.XTestFakeKeyEvent(self.display, keycode, False, 0):
            raise RuntimeError(f"XTestFakeKeyEvent release failed for keycode {keycode}")
        self.x11.XSync(self.display, False)

    def click(self, x: int, y: int) -> None:
        self.motion(x, y)
        if not self.xtst.XTestFakeButtonEvent(self.display, 1, True, 0):
            raise RuntimeError("XTestFakeButtonEvent press failed")
        self.x11.XSync(self.display, False)
        time.sleep(0.05)
        if not self.xtst.XTestFakeButtonEvent(self.display, 1, False, 0):
            raise RuntimeError("XTestFakeButtonEvent release failed")
        self.x11.XSync(self.display, False)


def open_quick_menu_through_emulation_menu(
    xtest: XTest, main_window: str
) -> None:
    subprocess.run(["wmctrl", "-ia", main_window], check=True)
    time.sleep(0.5)
    xtest.chord(("Alt_L", "e"))
    time.sleep(0.3)
    # The Quick Menu action has a stable D mnemonic in the Emulation menu.
    xtest.press("d")


def window_geometry(xid: str) -> tuple[int, int, int, int]:
    result = subprocess.run(
        ["xwininfo", "-id", x11_xid_arg(xid)],
        check=True,
        capture_output=True,
        text=True,
    )
    values = {}
    for key, pattern in (
        ("x", r"Absolute upper-left X:\s+(-?\d+)"),
        ("y", r"Absolute upper-left Y:\s+(-?\d+)"),
        ("width", r"Width:\s+(\d+)"),
        ("height", r"Height:\s+(\d+)"),
    ):
        match = re.search(pattern, result.stdout)
        if match is None:
            raise RuntimeError(f"No {key} geometry for X11 window {xid}")
        values[key] = int(match.group(1))
    return values["x"], values["y"], values["width"], values["height"]


def active_window_xid() -> int:
    result = subprocess.run(
        ["xprop", "-root", "_NET_ACTIVE_WINDOW"],
        check=True,
        capture_output=True,
        text=True,
        timeout=5,
    )
    match = re.search(r"window id # (0x[0-9a-fA-F]+|0)", result.stdout)
    if match is None:
        raise RuntimeError("X11 root has no parseable _NET_ACTIVE_WINDOW")
    return int(match.group(1), 0)


def exercise_declined_quit(
    xtest: XTest,
    main_window: str,
    render_xid: str,
    telemetry: Path,
    output,
) -> None:
    """Reproduce the focus/modal sequence that can strand the Wii hand cursor."""
    confirmations_opened = telemetry_count(telemetry, r"event=quit_confirmation_opened")
    confirmations_declined = telemetry_count(
        telemetry, r"event=quit_confirmation_closed response=No"
    )
    post_modal_activations = telemetry_count(
        telemetry, r"event=render_window_activate_enter .*modal=false"
    )

    # Switch from the separate render top-level back to Dolphin's main window, then use
    # the normal application close request. This is the same path as clicking the main
    # window close button and avoids window-manager-specific render activation requests.
    subprocess.run(["wmctrl", "-ia", main_window], check=True, timeout=5)
    time.sleep(0.5)
    subprocess.run(["wmctrl", "-ic", main_window], check=True, timeout=5)
    wait_telemetry(
        telemetry,
        r"event=quit_confirmation_opened",
        timeout=8,
        count=confirmations_opened + 1,
    )

    dialog_deadline = time.monotonic() + 5
    dialog_xid = 0
    excluded_windows = {0, int(main_window, 0), int(render_xid, 0)}
    while time.monotonic() < dialog_deadline:
        dialog_xid = active_window_xid()
        if dialog_xid not in excluded_windows:
            break
        time.sleep(0.1)
    else:
        raise RuntimeError("Quit confirmation never became the active X11 modal window")

    # Qt's standard No button exposes the N mnemonic. This rejects the close while
    # leaving the disposable emulation running, which is the safe equivalent of clicking No.
    xtest.chord(("Alt_L", "n"))
    wait_telemetry(
        telemetry,
        r"event=quit_confirmation_closed response=No",
        timeout=8,
        count=confirmations_declined + 1,
    )
    wait_telemetry(
        telemetry,
        r"event=render_window_activate_enter .*modal=false",
        timeout=8,
        count=post_modal_activations + 1,
    )
    output.write(
        f"switched away from render, activated quit dialog {hex(dialog_xid)}, and selected "
        "No; emulation remained active\n"
    )
    output.flush()


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
    transaction_phase = args.phase == "transaction-after-fix"
    focus_phase = args.phase.startswith("focus-") or transaction_phase
    phase_label = "after-fix" if transaction_phase else args.phase.removeprefix("focus-")
    artifact_prefix = (
        "quick-menu-transaction"
        if transaction_phase
        else ("quick-menu-focus" if focus_phase else "pointer-full-runtime")
    )
    telemetry = args.artifacts / f"{artifact_prefix}-{phase_label}.log"
    telemetry.unlink(missing_ok=True)
    output_path = args.artifacts / (
        "quick-menu-transaction-automation.log"
        if transaction_phase
        else ("quick-menu-focus-automation.log" if focus_phase else "automation-output.log")
    )
    window_tree_path = args.artifacts / (
        "quick-menu-transaction-window-tree.txt"
        if transaction_phase
        else ("quick-menu-focus-window-tree.txt" if focus_phase else "full-runtime-window-tree.txt")
    )
    screenshot_path = args.artifacts / (
        "quick-menu-action-after-fix.png"
        if transaction_phase
        else f"{artifact_prefix}-{phase_label}.png"
    )

    environment = os.environ.copy()
    environment.update(
        {
            "DOLPHIN_POINTER_E2E_LOG": str(telemetry),
            "DOLPHIN_POINTER_E2E_AUTO_BOOT": "1",
            "DOLPHIN_POINTER_E2E_AUTO_QUICK_MENU": "1",
            "QT_QPA_PLATFORM": "xcb",
        }
    )
    if transaction_phase:
        environment["DOLPHIN_POINTER_E2E_AUTO_QUICK_MENU_DELAY_MS"] = "30000"
        environment["DOLPHIN_POINTER_E2E_FORCE_QUICK_MENU_RECOVERY_FAILURE_AT"] = "6"
        environment["DOLPHIN_POINTER_E2E_FORCE_QUICK_MENU_FOCUS_TIMEOUT_AT"] = "7"
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
            text = wait_telemetry(
                telemetry,
                r"event=wiimote_ir_sample .*point_hidden=false .*ir_valid=true",
                timeout=10,
            )
            if transaction_phase:
                render_x, render_y, render_width, render_height = window_geometry(render_xid)
                if render_width <= 0 or render_height <= 0:
                    raise RuntimeError("Render window has empty X11 geometry")
                subprocess.run(["wmctrl", "-ia", x11_xid_arg(render_xid)], check=True)
                time.sleep(0.4)
                # Initial controller telemetry becomes valid while the Wii is still entering the
                # Health and Safety screen. Keep the render focused until that screen has had time
                # to expose its Press A state, then perform the single real left-click/A press.
                time.sleep(7)
                capture_window(
                    render_xid,
                    args.artifacts / "quick-menu-transaction-before-left-click.png",
                )
                xtest.click(
                    render_x + render_width // 2,
                    render_y + render_height // 2,
                )
                # The pre-click capture already proves that the Wii Menu and hand are present.
                # Leave enough time to observe the click without turning the test into a long
                # software-rendered delay, then exercise the focus/quit path.
                time.sleep(3)
                xtest.motion(
                    render_x + render_width // 2 + 40,
                    render_y + render_height // 2,
                )
                time.sleep(1)
                capture_window(
                    render_xid,
                    args.artifacts / "quick-menu-transaction-after-left-click.png",
                )
                output.write(
                    "confirmed a finite visible Wii Point with valid emulated IR, then sent "
                    "a real left click to the running render window\n"
                )
                output.flush()
                exercise_declined_quit(
                    xtest, main_window, render_xid, telemetry, output
                )
            if not transaction_phase:
                capture_window(
                    render_xid,
                    screenshot_path,
                )
            record_x11_windows(window_tree_path, f"{args.phase}: core running")

            if transaction_phase:
                # Don't click or focus the render after the declined quit: that could perform the
                # ordinary focus recovery and hide the condition being tested. Leave Dolphin on
                # its main window while the render/hand remains unfocused and invalid.
                subprocess.run(
                    ["wmctrl", "-ia", main_window], check=True, timeout=5
                )
                time.sleep(0.5)

            wait_telemetry(
                telemetry,
                r"event=quick_menu_action_automation",
                timeout=50 if transaction_phase else 10,
            )
            text = wait_telemetry(telemetry, r"event=quick_menu_post_event.*visible=true",
                                  timeout=12)
            quick_menu_xid = xid_from_telemetry(text, "xid")
            subprocess.run(["xwininfo", "-id", x11_xid_arg(quick_menu_xid)], check=True,
                           stdout=output, stderr=subprocess.STDOUT)
            capture_window(quick_menu_xid, args.artifacts / "quick-menu-full-runtime.png")
            if transaction_phase:
                initial_text = telemetry_text(telemetry)
                if len(
                    re.findall(r"event=quick_menu_qaction_triggered", initial_text)
                ) != 1:
                    raise RuntimeError(
                        "QAction automation did not trigger exactly one menu action"
                    )
                if len(
                    re.findall(r"event=quick_menu_mainwindow_request", initial_text)
                ) != 1:
                    raise RuntimeError(
                        "one QAction activation did not produce exactly one Quick Menu request"
                    )
            first_action_geometry = "reboot_geometry" if transaction_phase else "resume_geometry"
            resume_x, resume_y, resume_width, resume_height = geometry_from_telemetry(
                text, first_action_geometry
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
            backends_before_first_action = telemetry_count(
                telemetry, r"event=xinput2_backend_created"
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
                wait_telemetry(
                    telemetry,
                    r"event=quick_menu_recovery_complete",
                    timeout=10,
                    count=1,
                )
                if transaction_phase:
                    wait_telemetry(
                        telemetry,
                        r"event=xinput2_backend_created",
                        timeout=10,
                        count=backends_before_first_action + 1,
                    )
                    wait_telemetry(
                        telemetry,
                        r"event=quick_menu_reboot_requested "
                        r"operation=emulated_reset_button",
                        timeout=10,
                    )

            quick_menu_requests = len(
                re.findall(r"event=quick_menu_mainwindow_request", telemetry_text(telemetry))
            )
            quick_menu_qaction_triggers = len(
                re.findall(r"event=quick_menu_qaction_triggered", telemetry_text(telemetry))
            )
            # Exercise the supported Emulation menu path for the second lifecycle cycle.
            open_quick_menu_through_emulation_menu(xtest, main_window)
            text = wait_telemetry(
                telemetry,
                r"event=quick_menu_mainwindow_request",
                timeout=10,
                count=quick_menu_requests + 1,
            )
            if transaction_phase:
                wait_telemetry(
                    telemetry,
                    r"event=quick_menu_qaction_triggered "
                    r"object_name='actionDolphinQuickMenu' enabled=true",
                    timeout=2,
                    count=quick_menu_qaction_triggers + 1,
                )
                time.sleep(0.4)
                request_count_after_action = len(
                    re.findall(
                        r"event=quick_menu_mainwindow_request",
                        telemetry_text(telemetry),
                    )
                )
                if request_count_after_action != quick_menu_requests + 1:
                    raise RuntimeError(
                        "one Emulation menu QAction opened the Quick Menu more than once"
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
            if transaction_phase:
                capture_window(quick_menu_xid, screenshot_path)
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
                wait_telemetry(
                    telemetry,
                    r"event=quick_menu_recovery_complete",
                    timeout=10,
                    count=2,
                )

                # In the transaction phase, the first QAction cycle used the hard reboot fallback
                # immediately after the declined quit. Complete the non-destructive
                # coverage with Restore, Reconnect, and a second Resume.
                remaining_actions = (
                    (
                        (3, "restore_geometry"),
                        (4, "reconnect_geometry"),
                        (5, "resume_geometry"),
                    )
                    if transaction_phase
                    else ((3, "restore_geometry"), (4, "reconnect_geometry"))
                )
                for cycle, geometry_key in remaining_actions:
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
                    wait_telemetry(
                        telemetry,
                        r"event=quick_menu_recovery_complete",
                        timeout=10,
                        count=cycle,
                    )
                    if geometry_key == "reconnect_geometry":
                        wait_telemetry(
                            telemetry,
                            r"event=xinput2_backend_created",
                            timeout=10,
                            count=backends_before + 1,
                        )

                capture_window(render_xid, screenshot_path)
                if transaction_phase:
                    recoveries_before_failure = len(
                        re.findall(
                            r"event=quick_menu_recovery_complete",
                            telemetry_text(telemetry),
                        )
                    )
                    running_before_failure = len(
                        re.findall(r"event=core_running", telemetry_text(telemetry))
                    )
                    requests_before = len(
                        re.findall(
                            r"event=quick_menu_mainwindow_request",
                            telemetry_text(telemetry),
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
                        count=6,
                    )
                    failure_x, failure_y, failure_width, failure_height = (
                        geometry_from_telemetry(text, "resume_geometry")
                    )
                    xtest.click(
                        failure_x + failure_width // 2,
                        failure_y + failure_height // 2,
                    )
                    text = wait_telemetry(
                        telemetry,
                        r"event=quick_menu_recovery_failure_reopened "
                        r".*core_state=1 .*cursor_arrow=true "
                        r".*status='Wii pointer recovery failed",
                        timeout=10,
                    )
                    text = wait_telemetry(
                        telemetry,
                        r"event=quick_menu_post_event.*visible=true",
                        timeout=10,
                        count=7,
                    )
                    failure_menu_xid = xid_from_telemetry(text, "xid")
                    capture_window(
                        failure_menu_xid,
                        args.artifacts / "quick-menu-recovery-failure.png",
                    )
                    if len(
                        re.findall(
                            r"event=quick_menu_recovery_complete",
                            telemetry_text(telemetry),
                        )
                    ) != recoveries_before_failure:
                        raise RuntimeError("forced failure completed recovery unexpectedly")
                    if len(
                        re.findall(r"event=core_running", telemetry_text(telemetry))
                    ) != running_before_failure:
                        raise RuntimeError("forced failure resumed emulation")
                    record_x11_windows(
                        window_tree_path, f"{args.phase}: forced failure reopened"
                    )

                    # The failure menu is still a real Quick Menu session. Close it once more,
                    # with focus completion suppressed only for transaction 7, to prove the
                    # pre-recovery phase times out instead of stalling forever.
                    recovery_starts_before_focus_timeout = len(
                        re.findall(
                            r"event=quick_menu_recovery_started",
                            telemetry_text(telemetry),
                        )
                    )
                    focus_timeouts_before = len(
                        re.findall(
                            r"event=quick_menu_recovery_failed "
                            r"reason='focus/readiness timeout:",
                            telemetry_text(telemetry),
                        )
                    )
                    reopened_before_focus_timeout = len(
                        re.findall(
                            r"event=quick_menu_recovery_failure_reopened",
                            telemetry_text(telemetry),
                        )
                    )
                    text = telemetry_text(telemetry)
                    failure_x, failure_y, failure_width, failure_height = (
                        geometry_from_telemetry(text, "resume_geometry")
                    )
                    xtest.click(
                        failure_x + failure_width // 2,
                        failure_y + failure_height // 2,
                    )
                    wait_telemetry(
                        telemetry,
                        r"event=quick_menu_closed",
                        timeout=10,
                        count=7,
                    )
                    wait_telemetry(
                        telemetry,
                        r"event=quick_menu_recovery_failed "
                        r"reason='focus/readiness timeout:.*forced_timeout=true'",
                        timeout=10,
                        count=focus_timeouts_before + 1,
                    )
                    text = wait_telemetry(
                        telemetry,
                        r"event=quick_menu_recovery_failure_reopened "
                        r".*core_state=1 .*cursor_arrow=true "
                        r".*status='Wii pointer recovery failed",
                        timeout=10,
                        count=reopened_before_focus_timeout + 1,
                    )
                    text = wait_telemetry(
                        telemetry,
                        r"event=quick_menu_post_event.*visible=true",
                        timeout=10,
                        count=8,
                    )
                    focus_timeout_menu_xid = xid_from_telemetry(text, "xid")
                    capture_window(
                        focus_timeout_menu_xid,
                        args.artifacts / "quick-menu-focus-timeout.png",
                    )
                    if len(
                        re.findall(
                            r"event=quick_menu_recovery_started",
                            telemetry_text(telemetry),
                        )
                    ) != recovery_starts_before_focus_timeout:
                        raise RuntimeError(
                            "focus-timeout transaction started pointer recovery unexpectedly"
                        )
                    if len(
                        re.findall(r"event=core_running", telemetry_text(telemetry))
                    ) != running_before_failure:
                        raise RuntimeError("focus timeout resumed emulation")
                    record_x11_windows(
                        window_tree_path, f"{args.phase}: focus timeout reopened"
                    )
            record_x11_windows(window_tree_path, f"{args.phase}: after Quick Menu actions")

            if args.phase in ("after-fix", "focus-after-fix", "transaction-after-fix"):
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
        choices=(
            "before-fix",
            "after-fix",
            "focus-before-fix",
            "focus-after-fix",
            "transaction-after-fix",
        ),
    )
    parser.add_argument("--prepare-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    validate_automation_paths(args.source, args.profile, args.artifacts)
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
