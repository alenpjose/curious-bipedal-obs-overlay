# Curious Bipedal OBS Overlay

Native Windows x64 source plugin for OBS Studio 32.1.2. It adds **Curious Bipedal Session Overlay** to the OBS Sources menu and is designed for a normal landscape canvas plus an independent Aitum Vertical canvas.

## What it does

- Bottom-left session banner with the exact approved Curious Bipedal glyph.
- Safe-padded logo rendering that preserves the entire planet line and floating dot.
- Top-right clock, date, and elapsed timer panel.
- One shared opacity setting for panels, text, accents, and logo.
- Independent settings for every OBS source instance.
- Landscape timer binding to the confirmed main OBS stream start/stop events.
- Vertical timer binding to the named Aitum Vertical output's own start/stop signals.
- Manual start/pause/reset buttons and per-source OBS hotkeys.
- Responsive presets for 2560×1440 landscape and 1440×2560 vertical canvases, with custom dimensions available.

## End-user installation

1. Close OBS Studio.
2. Run `Curious-Bipedal-OBS-Overlay-Setup-<version>-windows-x64.exe`.
3. Restart OBS Studio.
4. In the normal landscape scene, choose **Sources → + → Curious Bipedal Session Overlay**, select **Create New**, and keep the Landscape preset with **Main OBS stream** timer binding.
5. In the Aitum Vertical scene, add another **new** Curious Bipedal source, select the Vertical preset, and enter the exact Aitum Vertical output name shown in Aitum's stream settings (normally `YouTube`).

Selecting an existing source intentionally shares the same instance. Use **Create New** for separate landscape and vertical settings.

## Compatibility

- Windows 10/11 x64
- OBS Studio 32.1.2
- Aitum Vertical (the vertical timer uses its public `aitum_vertical_get_stream_output` procedure)
- Aitum Multistream is compatible with the landscape arrangement: Twitch remains the main OBS stream and the landscape YouTube destination shares that same session timer.

The plugin does not link to Aitum. If Aitum Vertical is absent or the output name does not match, the overlay still renders and only the automatic vertical timer binding remains disconnected. The plugin retries every three seconds and also provides a **Reconnect to Aitum output** button.

## Building

The project is based on the official OBS plugin template build layout. The pinned `buildspec.json` downloads OBS Studio 32.1.2 sources and compatible OBS dependency bundles.

Local prerequisites:

- Windows x64
- Visual Studio 2022 with Desktop C++ tools and Windows 11 SDK 10.0.22621
- CMake 3.28 or newer
- PowerShell 7.2 or newer
- Inno Setup 6 (installer only)

Build commands from PowerShell 7:

```powershell
$env:CI = 'true'
./.github/scripts/Build-Windows.ps1 -Target x64 -Configuration Release
./.github/scripts/Package-Windows.ps1 -Target x64 -Configuration Release
& "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe" installer\curious-bipedal-obs-overlay.iss
```

GitHub Actions performs the same Windows x64 build, creates a portable ZIP and installer, and uploads both as workflow artifacts. A semantic-version tag also creates a GitHub Release.

## Verification checklist

- Launch OBS 32.1.2 with Aitum Vertical and Aitum Multistream installed.
- Add two **new** source instances; confirm edits do not leak between them.
- Inspect the logo at 2560×1440 and 1440×2560; the planet curve and dot must remain visible.
- Start/stop the main OBS stream; only the landscape-bound timer should follow it.
- Start/stop the named Aitum Vertical output; only the vertical-bound timer should follow it.
- Confirm timer buttons and hotkeys work in Manual mode.
- Restart OBS and confirm all source settings persist.
- Uninstall, restart OBS, and confirm the source no longer appears.

## Project status

This is an alpha source package. It has structural checks against the OBS 32.1.2 source tree, but the DLL and installer must be built and run in a real Windows OBS/Aitum installation before release.

## License and assets

Plugin code is GPL-2.0. The Curious Bipedal logo assets are supplied for this project by the brand owner; they are not relicensed for unrelated use.
