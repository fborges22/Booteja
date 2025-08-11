# La Booteja 🫙🪟🐧

This project is an EFI/UEFI utility for Windows to **list, select, and change boot entries and related options**. 

## Overview

La **Booteja** is a Windows-native CLI that manipulates UEFI boot configuration directly from userland. It focuses on the core boot variables that determine which EFI application or OS loader your firmware starts.

Typical tasks include:

* Viewing all `Boot####` entries and the current `BootOrder`
* Changing the default boot target
* Scheduling a one-time boot via `BootNext`
* Enabling/disabling or renaming boot entries
* Adjusting boot timeout and related options

Booteja is **not** a replacement for Windows Boot Manager or `bcdedit`—it complements them by working at the UEFI variable level.

> ⚠️ **Risk warning:** Modifying UEFI variables can make a system unbootable. Use at your own risk. Test in a VM or non‑critical machine first and ensure you have recovery media ready.

---

## Contents

* [Overview](#overview)
* [Features](#features)
* [How it works](#how-it-works)
* [Requirements](#requirements)
* [Build](#build)
* [Install](#install)
* [Usage](#usage)

  * [Common commands](#common-commands)
  * [Examples](#examples)
* [Troubleshooting](#troubleshooting)
* [FAQ](#faq)
* [Development](#development)
* [Roadmap](#roadmap)
* [Contributing](#contributing)
* [License](#license)

---

## Features

* 📜 Enumerate `Boot####` entries with labels, device paths, and status (active/hidden)
* 🔁 View and reorder `BootOrder`
* 🎯 Set default boot target (`BootOrder[0]`) or one-time target (`BootNext`)
* 🏷️ Rename entries (description field)
* 🚫 Enable/disable entries (toggle `LOAD_OPTION_ACTIVE` flag)
* ➕ Create or remove entries (advanced; see warnings below)
* 🧰 Export/import boot entries to a JSON file for backup/restore
* 🔒 Secure Boot–aware (read-only fallbacks when privileges are insufficient)

## How it works

Booteja uses Windows APIs to read/write UEFI variables under the **global variable namespace**:

* `BootOrder` – the array of 16‑bit IDs (e.g., `0000`, `0001`, …)
* `BootNext` – a one-time boot target (overrides `BootOrder` once)
* `Boot####` – per-entry structures describing a boot option (attributes, description, device path, optional data)

On Windows, accessing these variables requires the **`SeSystemEnvironmentPrivilege`** and an **elevated terminal**. Some OEM firmwares may restrict modifications when *Secure Boot* or certain lockdown features are enabled.

## Requirements

* Windows 10/11 x64 on a UEFI system
* Administrator rights (elevated terminal) and firmware that supports UEFI variable access
* **Build:** Visual Studio 2022 (Desktop development with C++) or **Build Tools for Visual Studio 2022** (MSBuild)
* **Windows SDK:** 10.0.22621 or newer

## Build

### Visual Studio (IDE)

1. Open `booteja.sln` in **Visual Studio 2022**.
2. Select **Release** and **x64**.
3. Build ▶️ **Build Solution**.

### MSBuild (CLI)

From a *Developer Command Prompt for VS 2022*:

```bat
msbuild booteja.sln /t:Build /p:Configuration=Release;Platform=x64
```

## Install

Copy the built binary (e.g., `booteja.exe`) anywhere on your `PATH` (e.g., `%ProgramFiles%\\Booteja`).

> You must run Booteja from an **elevated** terminal (Run as Administrator).

## Usage

```
booteja <command> [options]
```

### Common commands

* `list` — List all `Boot####` entries with IDs and attributes
* `order` — Show current `BootOrder`
* `order set <id[,id,...]>` — Set a new `BootOrder` sequence
* `select <id>` — Make `<id>` the first item in `BootOrder` (default boot)
* `next <id>` — Set `BootNext` for a one‑time boot
* `enable <id>` / `disable <id>` — Toggle entry active flag
* `rename <id> "New Description"` — Change the display label
* `create --file <efi-path> --desc "Label" [--data <hex> --active]` — Create a new `Boot####`
* `remove <id>` — Delete a boot entry
* `timeout [get|set <seconds>]` — Get or set the firmware boot timeout (if supported)
* `export <file.json>` / `import <file.json>` — Backup or restore entries
* `dump` — Raw dump of variables for diagnostics

Run `booteja help` or `booteja <command> --help` for detailed flags.

### Examples

List entries and current order:

```powershell
booteja list
booteja order
```

Set a new default boot (make `0003` first in `BootOrder`):

```powershell
booteja select 0003
```

Schedule a one-time boot into `0004` (next reboot only):

```powershell
booteja next 0004
```

Reorder two entries explicitly:

```powershell
booteja order set 0004,0001,0003,0002
```

Rename and disable an entry:

```powershell
booteja rename 0002 "Ubuntu NVMe"
booteja disable 0002
```

Backup and restore:

```powershell
booteja export boot-backup.json
# ...after changes or reinstall...
booteja import boot-backup.json
```

> 💡 **Tip:** If your firmware hides inactive entries, use `list --all` to include them.

## Troubleshooting

* **`Access is denied` / changes don’t persist**

  * Ensure the terminal is **Run as Administrator**.
  * Some devices require turning off *fast startup* or certain vendor security toggles before variable writes are allowed.
* **Entry IDs don’t appear after creation**

  * Power‑cycle the machine (full shutdown, not just reboot) so firmware rescans variables.
* **System won’t boot after changes**

  * Use your firmware setup utility to restore defaults or boot from recovery media and revert with `booteja import` (if you exported a backup).
* **Secure Boot blocks edits**

  * Some OEMs restrict modifications. Consider using read‑only commands and vendor tools if writes fail.

## FAQ

**Is this the same as `bcdedit`?**
No. `bcdedit` manages the Windows Boot Configuration Data. Booteja operates on the firmware’s UEFI variables (`Boot####`, `BootOrder`, `BootNext`). They are related, but separate layers.

**Does Booteja require EFI System Partition (ESP) drive letters?**
No for listing and most edits. For `create`, you must provide a valid *EFI application* path on the ESP (e.g., `\\\\?\\GLOBALROOT\\Device\\HarddiskVolume1\\EFI\\boot\\bootx64.efi`).

**Will this work with Secure Boot enabled?**
Reading usually works; writing may be restricted by firmware policy.

### Coding notes

* Prefer **RAII** for buffer management and handles
* Wrap Windows API calls (`GetFirmwareEnvironmentVariableExW`, `SetFirmwareEnvironmentVariableExW`)
* Parse/load options in `EFI_LOAD_OPTION` format
* Unit-test device path parsing where possible

## Roadmap

* [ ] Safer create/remove with firmware validation
* [ ] ESP path discovery helpers
* [ ] PowerShell completion script
* [ ] GUI wrapper for basic operations
* [ ] Signed releases

## Contributing

Issues and PRs are welcome. Please discuss significant changes first.

## License

MIT (see `LICENSE`).
