# Physical SD Device — Read Only on Linux

This feature is experimental and available only on desktop Linux. It gives Dolphin read-only access
to an unmounted SD card filesystem partition. Dolphin does not need, and must not be given, root
privileges.

## Identify the partition

Use `lsblk -f` to list block devices, filesystems, mount points, and filesystem UUIDs. You can also
query a known partition with `blkid <DEVICE_PARTITION>`. Check the device size, filesystem, and
reader/card identity carefully before configuring Dolphin.

Configure the filesystem partition, not the complete card device. A stable path is recommended:

```text
/dev/disk/by-uuid/<YOUR_SD_UUID>
```

The stable path remains useful if Linux assigns a different transient `/dev/sdX` name after a
reconnect. Dolphin retains the configured stable path; its startup diagnostics may resolve the
symlink temporarily to inspect the block device.

## Unmount without ejecting

The filesystem must not be mounted while Dolphin uses it. Unmount the partition using your desktop
environment or an appropriate `umount <MOUNT_POINT_OR_PARTITION>` command before starting
emulation.

Unmounting detaches the filesystem from Linux while leaving the card and reader connected. Ejecting
or safely removing the device may power down the reader or make the block device disappear, so do
not eject or physically remove it when preparing to use it in Dolphin.

Dolphin never unmounts devices automatically. Its mounted-device check is diagnostic; the later
read-only exclusive open remains the authoritative safety check.

## Narrow read-only permissions

Do not run Dolphin as root. Do not add your account to the broad `disk` group: that grants access to
many unrelated disks and can expose the entire system to accidental or malicious damage.

Instead, an administrator can create a dedicated group, such as `dolphin-sd`, add only the intended
account to it, and install a narrowly scoped udev rule for only the intended SD partition. This
repository does not install a group or rule.

Example rule structure, using placeholders:

```udev
SUBSYSTEM=="block", ENV{DEVTYPE}=="partition", ENV{ID_FS_UUID}=="<YOUR_SD_UUID>", \
  GROUP="dolphin-sd", MODE="0440"
```

`0440` grants read-only access to the device owner and the dedicated group. Only the account that
needs this feature should be added to `dolphin-sd`.

A production rule should ideally include stable reader or card identity properties in addition to
the filesystem UUID. Inspect the properties supplied by the particular reader and distribution,
and avoid rules that match every removable disk.

After an administrator installs or changes a rule, udev rules must be reloaded and the reader
reconnected (or the device event retriggered) before the permissions take effect. Distribution
commands and policy vary, so consult the distribution's udev documentation.

The rule affects host permissions only. Dolphin still opens the partition with read-only and
exclusive flags, rejects all emulated SD writes, and never falls back to a writable image when
physical-device mode fails.
