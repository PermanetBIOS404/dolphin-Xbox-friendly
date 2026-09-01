# Dolphin RWiN — Linux Release

> **Release status:** Linux release-candidate preparation is actively in progress. Final packaging, release audit, smoke testing, and GitHub release publication are still pending.

> Draft release notes. No public release has been published yet.

Dolphin RWiN is a fork of Dolphin Emulator. This first packaging target for the current Dolphin RWiN generation focuses on a portable Linux build and the Wii development and real-hardware workflows added by the fork.

## Highlights

- modern Wii input and mouse-pointer recovery
- Dolphin Quick Menu for input recovery and emulation controls
- Linux-focused physical Wii SD mount/unmount workflow
- Wii Export Assistant with USB Loader GX layout, validation, and rollback
- direct ISO and RVZ to WBFS export
- native Wii NKit v1 reconstruction
- NKit v1 support for legitimate zero-length regular FST files
- explicit USB Loader GX with d2x cIOS playable repair
- Dolphin RWiN Linux launcher and menu artwork integration
- desktop-launcher installer support
- runtime application and window icon support for Linux local and portable builds
- isolated Linux portable package

## NKit / d2x Notice

Strict reconstruction remains the default. Some Wii NKit v1 sources did not preserve enough original lower-level data to reproduce their archival hash hierarchy.

When explicitly selected, **Create a playable WBFS for USB Loader GX + d2x cIOS** regenerates the affected hash hierarchy and TMD content digest to produce an internally hash-consistent playable output. It preserves the original RSA signature bytes while recording that Nintendo authenticity is no longer preserved.

The repaired output is intended for USB Loader GX with d2x cIOS. It is **not** archival-original restoration, Nintendo-authentic metadata, intended for stock IOS, or a substitute for missing archival recovery information.

Use only game images you are legally entitled to use.

## Validation

Current PC-side validation includes Wii Sports + Wii Sports Resort (SP2E01), a real, user-owned Wii NKit v1 source containing legitimate zero-length regular FST files. It:

- completed NKit reconstruction;
- completed playable d2x WBFS repair;
- completed built-in exporter validation;
- passed independent `dolphin-tool` verification with only expected, low-severity characteristics of the repaired WBFS; and
- booted successfully in Dolphin.

Real-Wii acceptance through USB Loader GX with d2x cIOS is still pending and is not claimed by this release draft.

## Download

Planned portable archive:

```text
Dolphin-RWiN-Linux-Portable.tar.gz
```

The archive URL will be added only when the tested artifact is published.

## Upstream / License

Dolphin RWiN builds on [Dolphin Emulator](https://dolphin-emu.org/) and its [upstream source project](https://github.com/dolphin-emu/dolphin). Dolphin RWiN retains Dolphin's GNU General Public License, version 2 or later (GPLv2+) licensing and upstream attribution.
