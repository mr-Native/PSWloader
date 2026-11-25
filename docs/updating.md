# Updating PSWloader

Existing Windows 11 installations can be updated in-place without reinstalling Windows. The updater is version-independent, so you can jump directly from any older release to the latest one.

## Running an update

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

Replace `PSWloader.elf` only when a release says so (for example for new GameOS system software). Older ELFs (v1.0.5 and
earlier) do not have this: with them, replace `PSWloader.elf` once with the 2026-11-25 release.

Every run writes a log to:

```text
C:\PSWSetup\Update\PSWUpdate-<timestamp>.log
```

## Automatic update checks

Starting with v1.0.3, PSWloader installs a lightweight background task (`PSWloader Update Check`) that runs silently once at login when an internet connection is available.

- If you are offline or already on the latest version, nothing pops up.
- If a newer release is published on GitHub, a dialog offers three choices:
  - **Yes** — downloads the new `pswloader-vX.Y.Z-Updater.zip`, extracts it, and starts the updater.
  - **No** — skips for now and checks again on the next boot.
  - **Always disable updates** — turns off automatic checks permanently.

Checker logs are written to `C:\PSWSetup\Update\PSWUpdateCheck.log`.

### Disabling or re-enabling automatic checks

The checker reads its settings from:

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

To re-enable checks after clicking **Always disable updates**, open `C:\ProgramData\PSWloader\updater.json` in Notepad, change `"autoCheck": false` back to `"autoCheck": true`, and save. Your setting is preserved across future updates.

The currently installed version is tracked in `C:\ProgramData\PSWloader\version.json`.
