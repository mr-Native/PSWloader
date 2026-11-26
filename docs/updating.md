# Updating PSWloader

Existing Windows 11 installations can be updated in-place without reinstalling Windows. The updater is version-independent, so you can jump directly from any older release to the latest one.

## From the Dashboard

Open the **PSWloader Dashboard**, go to *Updates* and press *Check for updates*. If a newer release is there, *Download and
install* starts the updater. Turn on **Beta versions** to get beta releases too. They are experimental and can break
drivers or the installation, so use them at your own risk.

## Running an update by hand

1. Download `pswloader-vX.Y.Z-Updater.zip` from the [Releases](../../releases) page and extract the entire archive.
2. Run `PSWUpdater-vX.Y.Z.exe` as Administrator.
3. Confirm the version upgrade prompt.
4. When the update finishes, confirm the restart prompt to load the updated drivers.

## Firmware updates without replacing PSWloader.elf

Since the PSWloader.elf release of 2026-11-25 the ELF no longer has to be replaced for firmware updates. The updater puts
the new firmware on the EFI partition (`\PSWloader\firmware\pswloader-upl.bin`). At the next start the firmware built
into the ELF sees the newer one there and switches to it; Windows restarts then use it directly.

If the new firmware does not reach the Windows start twice in a row, it is ignored (`\PSWloader\firmware\update.txt`
says `failed`) and the built-in firmware keeps working. Deleting the `\PSWloader\firmware` folder goes back to the
built-in firmware.

Replace `PSWloader.elf` only when a release says so.

Every run writes a log to:

```text
C:\PSWSetup\Update\PSWUpdate-<timestamp>.log
```

## Automatic update checks

Starting with v1.0.3, PSWloader installs a lightweight background task (`PSWloader Update Check`) that runs silently once at login when an internet connection is available.

- If you are offline or already on the latest version, nothing pops up.
- If a newer release is published on GitHub, a window offers three choices:
  - **Update now** — downloads the new `pswloader-vX.Y.Z-Updater.zip`, extracts it, and starts the updater.
  - **Not now** — skips for now and checks again at the next sign-in.
  - **Turn off update checks** — turns off automatic checks permanently.
- With *Beta versions* switched on in the Dashboard, the check also offers pre-releases; the window then says *(beta)*.

Checker logs are written to `C:\PSWSetup\Update\PSWUpdateCheck.log`.

### Disabling or re-enabling automatic checks

Use the **Automatic update check** switch in the Dashboard under *Updates*. **Turn off update checks** in the update
window does the same. The setting is saved per Windows user and kept across updates.

The default for all users is in:

```text
C:\ProgramData\PSWloader\updater.json
```

Default contents:

```json
{
  "autoCheck": true,
  "repo": "mr-Native/PSWloader"
}
```

Set `"autoCheck": false` there to turn the check off for everyone who has not used the switch (needs administrator rights).

The currently installed version is tracked in `C:\ProgramData\PSWloader\version.json`.
