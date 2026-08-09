# Curious Bipedal OBS Overlay

Native Windows x64 source plugin for OBS Studio 32.1.2. It adds **Curious Bipedal Session Overlay** to the Sources menu and supports a normal landscape canvas plus an independent Aitum Vertical canvas.

## Features

- Bottom-left session banner using the approved Curious Bipedal glyph.
- A content-sized session banner that expands for longer titles without distorting its rounded corners.
- Safe-padded logo rendering that preserves the planet curve and floating dot.
- An 80% default overlay scale, adjustable per source from 50% to 180%.
- Top-right clock, date, and elapsed timer panel.
- Independent settings, timer state, and source hotkeys for every source instance.
- Main OBS stream start/stop binding for the landscape timer.
- Named Aitum Vertical stream-output start/stop binding for the vertical timer.
- Manual start, pause, and reset controls.
- Presets for 2560×1440 landscape and 1080×1920 vertical canvases, plus persistent custom dimensions such as 1440×2560.

## Installation

### Standard OBS installation

1. Close OBS Studio completely.
2. Run `Curious-Bipedal-OBS-Overlay-Setup-<version>-windows-x64.exe` as an administrator.
3. Start OBS Studio again.
4. Add **Curious Bipedal Session Overlay** from the Sources menu.

The installer writes to `C:\ProgramData\obs-studio\plugins\curious-bipedal-obs-overlay`, the shared Windows plugin location used by standard OBS installations.

The current alpha binaries are not code-signed, so Windows SmartScreen may show an unrecognized-publisher warning. Verify the download against `SHA256SUMS.txt` before allowing it to run.

### Portable OBS installation

The installer does not auto-detect portable OBS folders. Extract the plugin ZIP, then copy:

- `curious-bipedal-obs-overlay\bin\64bit\curious-bipedal-obs-overlay.dll` to `<portable OBS>\obs-plugins\64bit\`.
- The contents of `curious-bipedal-obs-overlay\data\` to `<portable OBS>\data\obs-plugins\curious-bipedal-obs-overlay\`.

## Source setup

1. In the normal scene, create a **new** source, choose the Landscape preset, and select **Main OBS stream**.
2. In the Aitum Vertical scene, create another **new** source, choose the Vertical preset, and select **Aitum Vertical output**.
3. Enter the Aitum output name exactly as it appears in Aitum's stream settings. The default `YouTube` is only an example.

Choosing an existing OBS source intentionally reuses that source and its settings. Choose **Create New** for independent landscape and vertical configuration.

Changing the layout preset applies that preset's dimensions once. Later width and height edits are stored on that source and are not overwritten when properties are reopened or another setting changes. Use **Reapply selected preset dimensions** only when you intentionally want to restore 2560×1440 or 1080×1920.

## Compatibility and behavior

- Windows 10 or 11 x64
- OBS Studio 32.1.2
- Aitum Vertical 1.6.x public procedure API
- Aitum Multistream

The plugin does not link against Aitum. It calls Aitum Vertical's public `aitum_vertical_get_stream_output` procedure using the configured canvas width, canvas height, and output name, then observes that output's own start/stop signals. It also reconciles the selected output once per second so it follows output objects that Aitum replaces during startup or reconnect. If Aitum Vertical is absent or the name/dimensions do not match, the overlay continues to render and retries automatically; only automatic vertical timer control is unavailable.

The landscape timer follows OBS's main streaming lifecycle. Aitum Multistream destinations that share the main OBS stream therefore share the landscape session timer; independently started Multistream outputs are not separate timer bindings.

## Timer controls and persistence

- Automatic bindings synchronize to the current main or Aitum output state when selected.
- Manual mode supports the source property buttons and per-source OBS hotkeys.
- Assign each source's Start/Pause and Reset bindings in **Settings → Hotkeys**; the plugin does not impose fixed global keys.
- Elapsed time, running state in Manual mode, source settings, and source hotkey assignments are stored with the scene collection.
- Reset sets elapsed time to zero without changing whether the timer is running.

## Building

Prerequisites:

- Windows x64
- Visual Studio 2022 with Desktop C++ tools and Windows SDK 10.0.22621 or newer
- CMake 3.28 or newer
- PowerShell 7.2 or newer
- Inno Setup 6 (installer only)

From PowerShell 7:

```powershell
$env:CI = 'true'
./.github/scripts/Build-Windows.ps1 -Target x64 -Configuration Release
./.github/scripts/Package-Windows.ps1 -Target x64 -Configuration Release
$version = (Get-Content buildspec.json -Raw | ConvertFrom-Json).version
& "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe" "/DMyAppVersion=$version" installer\curious-bipedal-obs-overlay.iss
./.github/scripts/Test-Package-Windows.ps1 -Configuration Release
```

GitHub Actions builds against the pinned OBS Studio 32.1.2 source and dependency bundle, verifies the x64 PE and portable ZIP structure, creates the Inno Setup installer, writes SHA-256 checksums, and uploads the DLL, ZIP, installer, and checksum file as one workflow artifact. A semantic-version tag publishes the distributable files as a GitHub Release.

## Release smoke-test checklist

- Launch OBS 32.1.2 with Aitum Vertical and Aitum Multistream installed and confirm the module-load log entry.
- Create two new sources and confirm edits, timer state, and hotkeys do not leak between instances.
- Inspect 2560×1440 and 1440×2560 canvases and confirm the planet curve and floating dot remain visible.
- Start/stop the main OBS stream and confirm only the main-bound timer follows it.
- Start/stop the selected Aitum Vertical output and confirm only the Aitum-bound timer follows it.
- Exercise manual buttons and hotkeys, restart OBS, and verify settings and elapsed state persist.
- Install and uninstall with OBS closed; restart after each operation and verify source availability changes as expected.

Do not publish a release as runtime-validated until this checklist passes on real OBS/Aitum binaries.

## License and assets

Plugin code is GPL-2.0. The Curious Bipedal logo assets are supplied for this project by the brand owner and are not relicensed for unrelated use.
