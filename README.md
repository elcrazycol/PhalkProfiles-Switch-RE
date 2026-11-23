# Phalk Profiles for Nintendo Switch

Homebrew client for [phalk.net/profiles](https://www.phalk.net/profiles/).

The application exports the play log of every local Switch profile, writes it as
JSON and uploads it to your Phalk Profiles account. An optional background
sysmodule does the same automatically when a game is closed.

## Requirements

1. A Nintendo Switch running Atmosphère.
2. An internet connection.
3. An account at [phalk.net/profiles](https://www.phalk.net/profiles/).
4. For building only: devkitPro with `switch-dev` and `switch-tools`.

## Build

```sh
export DEVKITPRO=/opt/devkitpro
make
```

`make` builds both binaries in the correct order:

* `PhalkProfiles.nro` — the interactive client;
* `PhalkProfiles_sysmodule.nsp` — the background uploader, which is copied into
  `romfs/sysmodule/sysmodule.nsp` and packed into the NRO's RomFS.

The sysmodule is no longer a pre-built blob: it is compiled from the same
`source/main.cpp` and configured by `sysmodule.json`.

To build only the sysmodule:

```sh
make sysmodule
```

## Installation

```text
sdmc:/switch/PhalkProfiles/PhalkProfiles.NRO
```

Launch the NRO through hbmenu.

> **Title override is recommended.** In applet mode (hbmenu launched from the
> Album) the process has a much smaller memory budget and fewer services. If
> anything fails, hold **R** while launching an installed game to open hbmenu in
> application mode, then start Phalk Profiles from there.

## Usage

1. `Set username` — enter your Phalk Profiles username.
2. `Set password` — enter your password.
3. `Export & upload` — exports the play data and uploads it.

The status line reports the result, and every step is also written to
`sdmc:/switch/PhalkProfiles/app.log`. `View log` shows the tail of that file.

### Automatic uploads (sysmodule, experimental)

The app installs the sysmodule **disabled**. Enabling it is a separate, explicit
step (`Enable sysmodule (experimental)`) and requires one reboot. Updating an
installed sysmodule also disables it again, so a new build can never start
silently on the next boot.

The sysmodule watches for the application process to exit, waits for the play
log to settle, exports the data and uploads it. Its log is written to:

```text
sdmc:/switch/PhalkProfiles/sysmodule.log
```

`Disable & remove sysmodule` removes the boot flag and the installed payload.

> **If Album/hbmenu hangs after enabling the sysmodule:** power off, mount the
> SD card on a PC (or use Hekate → Tools → USB Mass Storage), and delete:
>
> ```text
> sdmc:/atmosphere/contents/00FF0000D4A9E73C
> ```
>
> That only removes the sysmodule; it does not touch saves or the app. If you
> are unsure which sysmodule is at fault, rename `atmosphere/contents` to
> `atmosphere/contents.off`, boot once without any sysmodule, then add folders
> back one by one.

Manual paths used by the installer:

```text
sdmc:/atmosphere/contents/00FF0000D4A9E73C/exefs.nsp
sdmc:/atmosphere/contents/00FF0000D4A9E73C/toolbox.json
sdmc:/atmosphere/contents/00FF0000D4A9E73C/flags/boot2.flag
```

## Files

| Path | Purpose |
| --- | --- |
| `sdmc:/config/PhalkProfiles/config.txt` | username and password (plain text) |
| `sdmc:/switch/PhalkProfiles/export.json` | latest export |
| `sdmc:/switch/PhalkProfiles/app.log` | client log |
| `sdmc:/switch/PhalkProfiles/sysmodule.log` | sysmodule log |

The exported JSON contains `exportString`, `exportTimestamp` and a `users` array
with `name`, `id` and `titles` (`id`, `name`, `summary.firstPlayed`,
`summary.lastPlayed`, `summary.playtime`, `summary.launches`).

## Games missing from the export

Nintendo's PDM play log hides some entries from normal queries. The client now
reads the raw PDM event log as well, so titles with `PlayLogPolicy` 1 (`LogOnly`)
or 3 (`Closed`), and titles whose per-user statistics are missing, are exported
from the raw launch/focus/exit events. If the statistics database is stale, the
larger of the statistics value and the raw-log value is used for playtime and
launches.

Title names are read from every NACP language entry, and compressed NACP title
tables (`TitlesDataFormat` 1, used by newer games) are inflated with
`source/puff.c` (puff, Copyright (C) 2002-2013 Mark Adler, zlib license).

### Duplicate entries on the website

phalk.net keeps a game row per imported name. Renaming a game (for example from
`?`/`UNKNOWN GAME (id)` to its real title) can leave the old row behind. After
the first upload with the fixed names, delete the stale rows on the website
once; subsequent uploads keep using the stable name.

If games are still missing, run `PDM diagnostics` in the menu. It writes a
detailed report to:

```text
sdmc:/switch/PhalkProfiles/pdm_diag.log
```

The report contains the raw PDM range, every encountered title, the raw policy
bits, per-user raw playtime, the `pdm:qry` statistics with and without the
policy-3 flag, account-event UIDs, and the raw NACP language entries.

## Troubleshooting

* **The console shows `2168-0002` when exporting.** That is a crash of the
  homebrew process. Check `sdmc:/atmosphere/crash_reports/` and
  `sdmc:/switch/PhalkProfiles/app.log`; the log tells you which step failed.
* **`Export failed: accountInitialize failed` / `pdmqryInitialize failed`.**
  The current process is not allowed to use those services (common in applet
  mode). Use title override mode.
* **`Upload failed: ...`** — the export file is still written, so you can check
  the JSON and your credentials. The server is contacted over plain HTTP; see
  the security note below.
* **Game names show as `Unknown Game (id)`.** `nsInitialize` or the language
  service failed; the export itself is still valid.

## Security notes

* Credentials are stored in clear text in `config.txt`.
* The upload currently uses **plain HTTP** to `phalk.net` (with an IP fallback
  when DNS is unavailable). Anyone on the network path can see the credentials
  and the exported data. Do not use public Wi-Fi for this.
* The sysmodule runs with sysmodule privileges and writes to the SD card. It is
  built from the sources in this repository; read the code before installing it.

## License

GPLv3 — see `LICENSE`.
