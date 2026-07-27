#!/usr/bin/env python3

# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""AT-SPI smoke test for the production Dolphin Quick Menu host UI."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

try:
    from dogtail.config import config
    from dogtail.tree import root
except ImportError:
    print("dogtail is required: install the Python dogtail package.", file=sys.stderr)
    raise SystemExit(2)


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dolphin", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument(
        "--visual-check",
        type=Path,
        default=Path(__file__).with_name("verify_quick_menu_visual.py"),
    )
    return parser.parse_args()


def wait_for(description, timeout, getter):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            result = getter()
            if result is not None:
                return result
        except Exception as error:  # dogtail uses several lookup exception types
            last_error = error
        time.sleep(0.2)
    raise RuntimeError(f"Timed out waiting for {description}: {last_error}")


def screenshot(path):
    subprocess.run(["import", "-window", "root", str(path)], check=True, timeout=10)


def main():
    args = parse_arguments()
    dolphin = args.dolphin.resolve()
    if not dolphin.is_file():
        raise SystemExit(f"Dolphin executable not found: {dolphin}")

    args.artifacts.mkdir(parents=True, exist_ok=True)
    log_path = args.artifacts / "dolphin-quick-menu.log"
    before_path = args.artifacts / "before-quick-menu.png"
    after_path = args.artifacts / "quick-menu-visible.png"
    diff_path = args.artifacts / "quick-menu-visual-verified.png"
    result_path = args.artifacts / "quick-menu-result.json"

    config.searchCutoffCount = 30
    config.defaultDelay = 0.2

    with tempfile.TemporaryDirectory(prefix="dolphin-quick-menu-user-") as user_directory:
        environment = os.environ.copy()
        environment.update(
            {
                "DOLPHIN_QUICK_MENU_SMOKE_TEST": "1",
                "NO_AT_BRIDGE": "0",
                "QT_LINUX_ACCESSIBILITY_ALWAYS_ON": "1",
            }
        )
        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(
                [str(dolphin), "-u", user_directory],
                env=environment,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
            )

            try:
                application = wait_for(
                    "Dolphin AT-SPI application",
                    args.timeout,
                    lambda: root.application("dolphin-emu"),
                )
                main_window = wait_for(
                    "Dolphin Main Window",
                    args.timeout,
                    lambda: application.child(
                        name="Dolphin Main Window", roleName="frame", recursive=True
                    ),
                )
                screenshot(before_path)

                main_window.menu("Emulation").click()
                main_window.menuItem("Dolphin Quick Menu").click()

                quick_menu = wait_for(
                    "accessible Dolphin Quick Menu",
                    args.timeout,
                    lambda: application.child(name="Dolphin Quick Menu", recursive=True),
                )
                buttons = [
                    "Resume / Close Quick Menu",
                    "Restore Wii Pointer",
                    "Reconnect Mouse Input",
                    "Open Controller Settings",
                    "Stop Emulation",
                ]
                for name in buttons:
                    quick_menu.child(name=name, roleName="push button", recursive=True)

                x, y = quick_menu.position
                width, height = quick_menu.size
                if width <= 0 or height <= 0:
                    raise RuntimeError(f"Quick Menu has invalid AT-SPI geometry {width}x{height}")

                screenshot(after_path)
                subprocess.run(
                    [
                        sys.executable,
                        str(args.visual_check),
                        "--before",
                        str(before_path),
                        "--after",
                        str(after_path),
                        "--diff-output",
                        str(diff_path),
                        "--x",
                        str(x),
                        "--y",
                        str(y),
                        "--width",
                        str(width),
                        "--height",
                        str(height),
                    ],
                    check=True,
                )

                quick_menu.child(
                    name="Resume / Close Quick Menu", roleName="push button", recursive=True
                ).click()
                wait_for(
                    "Quick Menu to close",
                    args.timeout,
                    lambda: True if not quick_menu.showing else None,
                )
                result_path.write_text(
                    json.dumps(
                        {
                            "quick_menu_geometry": [x, y, width, height],
                            "buttons": buttons,
                            "closed": True,
                        },
                        indent=2,
                    ),
                    encoding="utf-8",
                )
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
