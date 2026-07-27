# Dolphin Quick Menu native GUI tests

These Linux-only harnesses exercise the production DolphinQt window hierarchy without booting a
game. Test-enabled builds recognize two internal environment variables:

- `DOLPHIN_QUICK_MENU_QT_TEST=1` runs the Qt Test integration suite and exits.
- `DOLPHIN_QUICK_MENU_SMOKE_TEST=1` keeps the isolated UI open for AT-SPI automation.

Both modes instantiate the real `MainWindow`, `MenuBar`, and `RenderWidget`. They substitute only
the Quick Menu's view of Core running/paused state, so no emulation or SD backend is started.
Always pass a new temporary user directory with `-u`; the supplied runners do this automatically.

## Native Qt Test under Xvfb

Configure Dolphin with `ENABLE_TESTS=ON`, build `dolphin-emu`, then run:

```sh
Tools/quick-menu/run_quick_menu_xvfb.sh \
  qt-test Build-raw-sd-phase1/Binaries/dolphin-emu
```

The equivalent optional build target is:

```sh
cmake --build Build-raw-sd-phase1 --target quick-menu-native-test -j4
```

The test triggers the real Emulation menu action and verifies lazy Quick Menu creation, visibility,
geometry, focus, pause-state transitions, host-button invocation, and closing.

## dogtail and visual smoke test

Required host tools:

- Python dogtail and an AT-SPI session
- Python OpenCV (`cv2`)
- ImageMagick `import`
- `dbus-run-session`, `Xvfb`, and `xvfb-run`

Run:

```sh
Tools/quick-menu/run_quick_menu_xvfb.sh dogtail \
  Build-raw-sd-phase1/Binaries/dolphin-emu \
  Build-raw-sd-phase1/QuickMenuArtifacts
```

dogtail opens **Emulation → Dolphin Quick Menu**, finds all five buttons through AT-SPI, records
before/after screenshots and Dolphin output, invokes Resume, and verifies closure. The OpenCV
helper compares pixels within the Quick Menu's AT-SPI bounds. This catches the native-surface
regression where Qt reports `isVisible()` but the embedded widget remains visually occluded: the
before/after pixel thresholds fail even if logical QWidget assertions pass.

No test uses the normal Dolphin user directory, boots a title, or selects an SD backend.
