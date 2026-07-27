#!/usr/bin/env python3

# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify that opening Quick Menu materially changed pixels over the render surface."""

import argparse
from pathlib import Path
import sys

try:
    import cv2
except ImportError:
    print("OpenCV is required: install the Python cv2 module.", file=sys.stderr)
    raise SystemExit(2)


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--diff-output", type=Path, required=True)
    parser.add_argument("--x", type=int, required=True)
    parser.add_argument("--y", type=int, required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--minimum-changed-ratio", type=float, default=0.05)
    parser.add_argument("--minimum-mean-difference", type=float, default=6.0)
    return parser.parse_args()


def main():
    args = parse_arguments()
    before = cv2.imread(str(args.before), cv2.IMREAD_COLOR)
    after = cv2.imread(str(args.after), cv2.IMREAD_COLOR)
    if before is None or after is None:
        raise SystemExit("Could not read one or both screenshots")
    if before.shape != after.shape:
        raise SystemExit(f"Screenshot dimensions differ: {before.shape} versus {after.shape}")

    screen_height, screen_width = before.shape[:2]
    left = max(0, args.x)
    top = max(0, args.y)
    right = min(screen_width, args.x + args.width)
    bottom = min(screen_height, args.y + args.height)
    if right <= left or bottom <= top:
        raise SystemExit("Quick Menu accessibility geometry does not intersect the screenshot")

    before_crop = before[top:bottom, left:right]
    after_crop = after[top:bottom, left:right]
    difference = cv2.absdiff(before_crop, after_crop)
    gray_difference = cv2.cvtColor(difference, cv2.COLOR_BGR2GRAY)
    changed_ratio = float((gray_difference >= 12).sum()) / float(gray_difference.size)
    mean_difference = float(gray_difference.mean())

    annotated = after.copy()
    cv2.rectangle(annotated, (left, top), (right - 1, bottom - 1), (0, 255, 0), 2)
    args.diff_output.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(args.diff_output), annotated):
        raise SystemExit(f"Could not write {args.diff_output}")

    print(
        f"Quick Menu visual change: ratio={changed_ratio:.4f}, "
        f"mean_difference={mean_difference:.2f}, region={right-left}x{bottom-top}"
    )
    if changed_ratio < args.minimum_changed_ratio:
        raise SystemExit(
            f"Only {changed_ratio:.4f} of overlay pixels changed; "
            f"required {args.minimum_changed_ratio:.4f}"
        )
    if mean_difference < args.minimum_mean_difference:
        raise SystemExit(
            f"Mean overlay difference {mean_difference:.2f} is below "
            f"{args.minimum_mean_difference:.2f}"
        )


if __name__ == "__main__":
    main()
