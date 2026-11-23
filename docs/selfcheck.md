# Support zip (PSWSelfCheck)

Double-click `C:\PSWloader\selfcheck\PSWSelfCheck.cmd` and confirm the administrator prompt. It takes about a minute and
writes one file:

`C:\PSWloader\selfcheck\results\PSW-selfcheck-<date>_<time>.zip`

Send that zip with your support request. It contains:

- `summary.txt` – PSWloader version, when it was installed and updated, Windows version and install date, GPU and power driver state
- devices and drivers (status, problem codes, driver versions), `pnputil` driver list
- registry of the PSWloader drivers and the PS5 PCI devices
- boot configuration (`bcdedit`), disks, partitions, network adapters
- UEFI system check (`PS5DIAG.LOG`) and the firmware logs from the EFI partition and from USB drives, also as readable text
- System, Application and Setup event logs of the last 14 days (`.evtx` plus a text list of errors and warnings)
- PSWloader setup and update logs, Windows setup and driver install logs, the newest crash dumps
- `dxdiag` report
- the background log (`watch/`, see below)

Nothing on the system is changed. The tool only gives the EFI partition a temporary drive letter and removes it again.

## Background log (PSWWatch)

A small task, *PSWloader Watch*, runs at every start and then once a minute. It writes to
`C:\PSWloader\logs\watch\watch.log`:

- new warnings and errors from the System and Application event logs, and every start, shutdown, restart request,
  unexpected shutdown and bug check
- every device that gets a problem code (for example the GPU with Code 43, a USB device that drops off) and when it
  is fine again
- at each start: when Windows was last seen running before it, so a freeze or a sudden power-off shows up as a gap
- a copy of the firmware logs of each start in `watch\uefi\` (the EFI partition keeps only the latest)

The log is rotated at 5 MB and keeps 6 files; the newest 30 firmware log copies are kept. The same task keeps the other
automatic PSWloader logs small (in `C:\ps5-work`, `C:\PSWSetup\Update`, `C:\ProgramData\PSWloader\logs`): at 5 MB a log
becomes `<name>.old` and replaces the previous one. The newest 10 support zips are kept. The support zip includes the
whole folder, so a problem that happened hours or days earlier can still be traced.
