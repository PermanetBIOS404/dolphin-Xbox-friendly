# Dolphin RWiN

**Dolphin RWiN is a fork of Dolphin Emulator focused on modern Wii input, development/testing workflows, physical Wii integration, and easier movement of games between Dolphin and real Wii hardware.**

[Upstream Dolphin Homepage](https://dolphin-emu.org/) | [Upstream Source](https://github.com/dolphin-emu/dolphin) | [Dolphin Wiki](https://wiki.dolphin-emu.org/) | [Forums](https://forums.dolphin-emu.org/) | [Linux Build Documentation](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Linux)

Dolphin RWiN builds on the upstream [Dolphin Emulator](https://dolphin-emu.org/) project. It does not replace Dolphin or claim the emulator foundation created by Dolphin's contributors. This fork adds RWiN-specific input, testing, physical-Wii, and game-export workflows while preserving upstream attribution and GPLv2+ licensing.

RWiN means **Reconnecting Wii Networks**. The intended workflow spans:

```text
Modern PC controls
    → Dolphin Wii testing
    → physical Wii storage
    → USB Loader GX / homebrew workflows
    → real Wii hardware
```

The current release focus is **Linux**. Windows release preparation is planned after the Linux release has been built and validated.

## Major Features

### Modern Wii Input

Dolphin RWiN expands the ways modern PC controls can be used for Wii software and development:

- Xbox- and modern-controller-friendly Wii input
- mouse-based Wii pointer support
- a keyboard-and-mouse testing fallback/profile
- explicit pointer recovery
- controller and input reconnection tools
- the host-side Dolphin Quick Menu

### Dolphin Quick Menu

Open the Quick Menu from **Emulation → Dolphin Quick Menu**. Its current actions include:

- Resume / Close Quick Menu
- Reconnect Mouse Input / Controllers
- Restore Wii Pointer
- Reboot Emulation + Reconnect Input
- Controller Settings
- Stop Emulation

A dependable application-wide shortcut remains deferred because shortcut delivery was not reliable enough across tested Linux desktop environments. The menu itself remains available through Dolphin's Emulation menu.

## Physical Wii SD Support

Dolphin RWiN provides Linux-focused support for using a physical Wii SD card with Dolphin. The Wii settings interface distinguishes between:

- **Virtual SD** — Dolphin uses an emulated SD-card image.
- **Physical SD** — Dolphin directly accesses a connected physical SD device.

For Physical SD workflows, Dolphin RWiN can display device detection and mount status. The **Unmount** control hands the device from normal Linux filesystem access to Dolphin direct access without ejecting or physically removing it. After emulation releases the device, the **Mount** control returns it to Linux filesystem access.

Physical-device support is currently Linux-focused. Do not remove an SD card while Dolphin is using it, and confirm the correct device before changing its mount state.

## Wii Export Assistant

The Wii Export Assistant prepares supported Wii disc images for USB Loader GX. From the Game List:

```text
Right-click a Wii game
    → Export for USB Loader GX...
```

The workflow provides:

- destination inspection
- expected-size planning before export
- USB Loader GX directory layout
- FAT32 WBFS splitting when required
- no-overwrite behavior
- staging and rollback
- cancellation
- output validation
- cleanup of temporary or failed output

Canonical output uses this layout:

```text
wbfs/
└── Game Title [ID6]/
    └── ID6.wbfs
```

## Supported Wii Export Sources

### ISO

Conventional Wii ISO images use the native conventional-disc WBFS analysis and writing path.

### RVZ

RVZ images use Dolphin's DiscIO-backed source path and can be exported without first requiring the user to create a separate ISO manually.

### Wii NKit v1

Dolphin RWiN includes a native Wii NKit v1 reconstruction path:

```text
Wii NKit v1
    → native reconstruction
    → virtual conventional Wii disc
    → AnalyzeWbfs
    → existing WBFS writer
    → USB Loader GX layout
```

The original NKit source remains read-only. Reconstruction is exposed as a bounded virtual conventional-disc view for the existing WBFS pipeline rather than rewriting the source image.

## Retail NKit Reconstruction

The current Wii NKit v1 implementation includes:

- NKit v1 metadata validation
- handling for removed update partitions
- a clear distinction between playable and archival recovery
- deterministic junk and filler reconstruction
- partition reconstruction
- H0/H1/H2/H3 generation
- Wii partition encryption
- partial final partition-group handling
- retail-scale indexing
- nested FST traversal
- canonical and physical file ordering
- directory-aware gaps
- bounded random-access reconstruction
- full WBFS-used-block preflight validation

The implementation is format-driven and is not specific to individual game IDs.

## d2x Playable Repair

Some Wii NKit v1 sources cannot reproduce their original hash hierarchy because required original lower-level data was not preserved. Strict behavior remains the default: when the required archival information is unavailable, Dolphin RWiN fails closed instead of silently presenting a reconstructed result as original.

When appropriate, the UI explicitly offers:

**Create a playable WBFS for USB Loader GX + d2x cIOS**

When selected, this explicit repair mode:

1. reconstructs canonical game data;
2. regenerates affected H0/H1/H2/H3 data;
3. replaces affected H3 entries in the virtual reconstructed view;
4. recalculates the TMD content digest over the repaired H3 table; and
5. preserves the original RSA signature bytes while recording that Nintendo authenticity is no longer preserved.

A repaired output **is**:

- intended for USB Loader GX with d2x cIOS
- intended for playable use
- internally hash-consistent

A repaired output **is not**:

- archival-original restoration
- Nintendo-authentic metadata
- intended for stock IOS
- a substitute for missing archival recovery information

This repair is an explicit compatibility path, not an archival claim.

## Real-World Validation

Current proven PC-side validation includes:

- a real, user-owned Wii NKit v1 source processed through the reconstruction pipeline
- a real repaired NKit-to-WBFS export reaching completion
- the generated WBFS reopening successfully through Dolphin
- the resulting WBFS booting successfully in Dolphin

Real-hardware acceptance through USB Loader GX with d2x cIOS is a separate validation stage and remains pending. It must not be inferred from successful Dolphin-side validation.

Use only game images you are legally entitled to use.

## RWiN Ecosystem

**RWiN** means **Reconnecting Wii Networks**.

- **Dolphin RWiN** is the overall Dolphin fork, emulator, and development platform.
- **RWiN Bridge** is a subsystem/project intended to exist within the Dolphin RWiN ecosystem.

RWiN Bridge is intended to explore Dolphin-to-physical-Wii communication, development and testing bridges, telemetry and diagnostic possibilities, and future real-hardware integration. This README does not claim that unfinished Bridge functionality ships in the current Linux release.

Related concepts may progress independently and are not necessarily bundled with this release:

- **RWiN Adelaide** — diagnostics and troubleshooting
- **RWiN Rewired** — modern-controller-to-Wii-input translation
- **RWiN Vigil** — a Wii homebrew hub

## Linux Release

Linux is the first packaging target for this generation of Dolphin RWiN. The planned portable package structure is:

```text
Binaries/
├── dolphin-emu
├── Sys/
└── portable.txt
```

The planned archive name is:

```text
Dolphin-RWiN-Linux-Portable.tar.gz
```

`portable.txt` tells Dolphin to keep this package's user environment separate from the normal system Dolphin configuration, making the archive suitable for release validation and isolated testing.

## Linux Build Instructions

See Dolphin's [Linux build documentation](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Linux) for required toolchain and library packages. Dolphin RWiN uses the same upstream-style CMake workflow, with Ninja shown here:

```sh
git submodule update --init --recursive

cmake -S . -B Build -GNinja \
  -DLINUX_LOCAL_DEV=true \
  -DENABLE_ANALYTICS=OFF \
  -DENABLE_DISCORD_PRESENCE=OFF

cmake --build Build --parallel

touch Build/Binaries/portable.txt
```

With `LINUX_LOCAL_DEV=true`, the build automatically copies `Data/Sys` to `Build/Binaries/Sys`, beside the `dolphin-emu` executable. It also produces the launcher artwork at `Build/Binaries/dolphin-rwin.png` and an installer at `Build/Binaries/install-desktop-launcher.sh`. The `portable.txt` step remains necessary for the portable-package workflow.

Run `./Build/Binaries/install-desktop-launcher.sh` to create or update the per-user Dolphin RWiN launcher; no `sudo` is required. This configures the launcher/menu shortcut artwork and is separate from the running-window icon mechanism described below. The launcher uses absolute paths to the current extracted or build directory, so rerun the installer after moving the whole directory.

Lower build parallelism may be useful on resource-constrained machines, but it is not a Dolphin RWiN release requirement.

### Linux Generic Gear / Missing Window Icon

On Linux, the desktop or menu shortcut can show its configured icon while the running Dolphin window and taskbar entry show a generic gear. The launcher icon and the running window icon are separate.

Dolphin RWiN now handles the runtime icon automatically: it assigns `Resources::GetAppIcon()` to the `QApplication`, using the existing Dolphin application logo, and local-development builds place the required `Sys` resources beside the executable. Users running a current Dolphin RWiN build should not need to copy resources or configure the window icon manually. This validates the runtime icon mechanism; custom Dolphin RWiN artwork will be handled separately.

To diagnose an older or incorrectly packaged build, find the top-level Dolphin window:

```sh
wmctrl -lx | grep -i dolphin
```

Then inspect the returned window ID:

```sh
xprop -id <WINDOW_ID> WM_CLASS _NET_WM_NAME _NET_WM_ICON
```

The affected window reported `WM_CLASS` as `"dolphin-emu", "dolphin-emu"` and included `_NET_WM_ICON: not found.` For an older or mispackaged `LINUX_LOCAL_DEV` build, the expected runtime icon resource is:

```text
Build/Binaries/Sys/Resources/dolphin_logo.png
```

The preferred permanent fix is to rebuild from current Dolphin RWiN so the CMake post-build rule supplies `Build/Binaries/Sys` automatically. For an old local build only, ensuring that `Data/Sys` is present beside the executable as `Sys` can be used as a temporary diagnostic workaround; it is not the current build behavior.

## Windows

Windows release preparation comes after the Linux release is built, tested, and published. Existing Windows CI artifacts are development artifacts and are not being presented as a validated public Dolphin RWiN release in this pass.

## Known Limitations

- The Quick Menu's dependable application-wide shortcut remains deferred; use **Emulation → Dolphin Quick Menu**.
- Physical SD work is Linux-focused and must fail closed when safe direct-device access cannot be established.
- Current native reconstruction targets supported Wii NKit v1 layouts.
- GameCube NKit is not currently part of this reconstruction feature.
- NKit 2 is not currently part of this reconstruction feature.
- Some archival cases still require external recovery data that the source did not preserve.
- d2x playable repair is not intended for stock IOS and does not restore Nintendo-authentic metadata.
- Real-Wii validation may lag behind Dolphin-side validation; USB Loader GX with d2x cIOS acceptance is currently pending.

## Upstream Dolphin

Dolphin RWiN inherits Dolphin's emulator foundation and adds RWiN-specific functionality. The upstream project and its contributors remain the source of the emulator core on which this fork is built.

- [Dolphin Emulator homepage](https://dolphin-emu.org/)
- [Upstream Dolphin source](https://github.com/dolphin-emu/dolphin)
- [Dolphin Wiki](https://wiki.dolphin-emu.org/)
- [Dolphin Forums](https://forums.dolphin-emu.org/)
- [Building Dolphin on Linux](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Linux)

## License

Dolphin and Dolphin RWiN are licensed under the terms of the GNU General Public License, version 2 or later (**GPLv2+**). See [license.txt](license.txt) for the full license text and retain upstream copyright and licensing notices when redistributing modified builds.

## Project Status

Dolphin RWiN is an actively developed fork. The Linux portable release is the current release effort; Windows release work comes next after Linux validation.
