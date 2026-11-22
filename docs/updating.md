# Updating PSWloader

Existing Windows 11 installations can be updated in-place without reinstalling Windows. The updater is version-independent, so you can jump directly from any older release to the latest one.

## Running an update

1. Download `pswloader-vX.Y.Z-Updater.zip` from the [Releases](../../releases) page and extract the entire archive.
2. Run `PSWUpdater-vX.Y.Z.exe` as Administrator.
3. Confirm the version upgrade prompt. If the release also updated `PSWloader.elf`, the updater will let you know before starting — you can replace `PSWloader.elf` (and `PSWloader.elf.json`) in your PS5 Payload Manager after the reboot at the end of the update.
4. When the update finishes, confirm the restart prompt to load the updated drivers.

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
