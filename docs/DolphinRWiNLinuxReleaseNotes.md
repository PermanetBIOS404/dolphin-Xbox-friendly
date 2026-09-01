# Dolphin RWiN — Linux Release

> **Release status:** The GitHub-built Linux release candidate has completed packaging, release audit, and target-laptop portability smoke testing. GitHub release publication is still pending.

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

### GitHub-built Linux portable candidate

GitHub Actions Linux run `33470341807` successfully built commit `c36b9e287807c294d25206be7e909e6752afb4f3`. Configure, build, portable packaging, privacy audit, shared-library dependency audit, checksum generation, and artifact upload all passed.

The public Linux release configuration uses `-DENABLE_LLVM=OFF`. LLVM integration provides optional disassembler functionality; it is not an emulator requirement. Leaving it enabled caused an earlier GitHub artifact to depend on the runner-specific `libLLVM-17.so.1`, so it is disabled for the portable release rather than imposing that library version on users.

The corrected artifact was downloaded and tested on the target Linux Mint laptop. Its archive SHA-256 is:

```text
27a94ad0dbbd0611abf6e78e4182aa69bbdbb93989bbb85246475b3d153d4354
```

On the target laptop:

- `dolphin-emu`, `dolphin-emu-nogui`, and `dolphin-tool` each had zero missing shared libraries;
- none of the three executables retained an LLVM shared-library dependency;
- the corrected GitHub-built `dolphin-emu` launched successfully;
- the build was installed at `~/.local/share/Dolphin-RWiN/releases/c36b9e2878/`;
- the desktop/application launcher was updated to that build and launched successfully;
- existing standard Dolphin configuration and profile data continued to work; and
- the separate Dolphin RWiN VirtualWii profile remained untouched.

Real-Wii acceptance through USB Loader GX with d2x cIOS is still pending and is not claimed by this release draft.

## Download

Planned portable archive:

```text
Dolphin-RWiN-Linux-Portable.tar.gz
```

The archive URL will be added only when the tested artifact is published.

## Upstream / License

Dolphin RWiN builds on [Dolphin Emulator](https://dolphin-emu.org/) and its [upstream source project](https://github.com/dolphin-emu/dolphin). Dolphin RWiN retains Dolphin's GNU General Public License, version 2 or later (GPLv2+) licensing and upstream attribution.
