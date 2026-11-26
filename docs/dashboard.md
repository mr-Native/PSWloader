# PSWloader Dashboard

The Dashboard is installed with PSWloader and opens from the Start menu or the desktop, and at every sign-in. It needs no
administrator rights.

## Overview

Your console model (PS5 Fat, Slim or Pro), the PSWloader, firmware and Windows versions, and live graphs for processor,
graphics, memory and temperature with fan speed. Below that: hardware details and all drives with their free space.

## Health

PSWloader checks itself at every Windows start, every six hours and when you press **Check now**. Problems are shown in
red, notes in yellow. While a check runs, the results of the last one are shown greyed out as *Pending*.

**Troubleshoot** (experimental) is only offered when the check found something it can fix. It asks before it changes
anything and can:

- turn test signing back on
- reinstall a PS5 driver or restart a device that reports a problem
- set up the internal M.2 SSD again
- restart PSWloader background tasks and services that stopped
- adapt a Windows installation that was moved to another console or SSD

Some fixes need a restart. If a problem stays, create a support zip and open an issue.

## Tools

- **UEFI Boot Manager:** restart straight into the boot menu to start another drive or change PSWloader settings.
- **Support zip:** collects all logs into one zip for a GitHub issue. Names, Wi-Fi names, MAC addresses and serial
  numbers are removed.
- **What's new**, **Report a problem**, **Support the project**.
- **Start with Windows:** open the Dashboard when you sign in (on by default).

## Updates

**Check for updates** looks for a new release on GitHub. **Download and install** starts the updater (administrator
rights needed).

- **Automatic update check:** look for updates at every sign-in.
- **Beta versions:** also offer beta releases. They are experimental and can break drivers or the installation. Use at
  your own risk.
