# Xbox-Friendly Dolphin Fork (Windows) — Quick Setup

This fork includes Windows-focused defaults to make an Xbox controller (Xbox One / Series) behave like an **emulated Wii Remote** (plus Nunchuk) with minimal re-binding.

## Easiest way (no commands): download a prebuilt zip

If this repo has a **Release** (recommended) or a recent **Actions build artifact**, you can download and run Dolphin without using any command line.

### Option A: GitHub Releases (best)

1. Open the repo on GitHub.
2. Click **Releases** (right side of the page).
3. Download the latest `Dolphin-Xbox-friendly-Windows.zip` (or similarly named zip).
4. Unzip it anywhere (Desktop is fine).
5. Run `Dolphin.exe`.

### Option B: GitHub Actions artifact (also no commands)

1. Open the repo on GitHub.
2. Click the **Actions** tab.
3. Open the latest successful run of the **Build Windows (Xbox-friendly)** workflow.
4. Scroll to **Artifacts** and download the zip.
5. Unzip it anywhere.
6. Run `Dolphin.exe`.

If you don’t see Releases or Actions artifacts, the maintainer hasn’t published a build yet — use the “Build it yourself” section below.

## Build it yourself (Windows)

## 1) Prereqs (Windows)

- Windows 10 (1903+) or Windows 11
- Visual Studio 2022 (or Build Tools) with C++ workload + Windows SDK
- Git

## 2) Build

Download the source code zip from GitHub, unzip it, then open `Source/dolphin-emu.sln` in Visual Studio and build:
- Configuration: `Release`
- Platform: `x64`

## 3) Pair/Connect the Xbox Controller (before launching Dolphin)

Connect the controller first, then start Dolphin.

- USB: plug in the cable
- Bluetooth: Windows Settings → Bluetooth & devices → Add device → Bluetooth → “Xbox Wireless Controller”
- Xbox Wireless Adapter: plug in adapter and pair

## 4) Enable “Emulated Wii Remote” and use the included profile

1. Dolphin → `Controllers`
2. Under `Wii Remotes`, set `Wii Remote 1` to **Emulated Wii Remote**
3. Click `Configure`
4. In the `Profile` dropdown, pick:
   - `Xbox Controller (XInput-SDL) (Stock)`
5. Click `Load`

Tip: if you don’t want the Nunchuk, change `Extension` to `None` inside the mapping UI.

## 5) What this fork changes (why it’s easier)

On Windows, Dolphin’s “Default” mappings often start from `Keyboard Mouse` because it’s treated as the default input device.
This fork tweaks defaults so that, when Dolphin starts and a real controller is connected, it prefers a gamepad device:

- Prefer `SDL` first, then `XInput`, then non-virtual `DInput`
- Applies Xbox-style defaults for:
  - Emulated Wii Remote (IR pointing on right stick, buttons mapped)
  - GameCube Pad
  - GBA Pad

## Troubleshooting

- **Controller doesn’t appear in Dolphin at all**
  - Confirm Windows sees it in Settings → `Bluetooth & devices` (or `Game Controllers` / `joy.cpl`).
  - Try USB (simplest) to rule out Bluetooth issues.
- **Bindings still look like keyboard/mouse**
  - Quit Dolphin, connect the controller, relaunch Dolphin.
  - In the controller config, pick the included profile and click `Load`.
- **Want motion/gyro**
  - Xbox controllers generally don’t provide gyro; pointing is mapped to the right stick in this fork/profile.
