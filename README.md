<p align="center">
  <img src="res/reticle.png" width="96" height="96" alt="Reticle logo" />
</p>

<h1 align="center">Reticle</h1>

<p align="center"><b>A lightweight in-game performance overlay and crosshair for Windows.</b></p>

<p align="center">
  <a href="../../releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/larosa023/Reticle?color=ff6a3d&label=download" /></a>
  <img alt="Windows 10/11" src="https://img.shields.io/badge/Windows-10%20%7C%2011-1f1f26" />
  <img alt="MIT license" src="https://img.shields.io/badge/license-MIT-1f1f26" />
</p>

Reticle shows FPS, frame times and your hardware stats over your game, like the RivaTuner/Afterburner overlay but without the clutter. It can also draw a custom crosshair. It comes up on its own when a game is in front and gets out of the way when you alt-tab.

> **Works over borderless and windowed games** (most modern games default to borderless). Reticle draws in its own see-through window on top of the game, so a game in **exclusive fullscreen** hides it. Pick *Borderless* or *Windowed fullscreen* in the game's display settings.

## Features

- **Overlay**: FPS, frame time with a small live graph, 1% and 0.1% lows, CPU usage and temperature, GPU usage, temperature, clock, power and VRAM (NVIDIA), RAM, the time and session length. Each one can be turned on or off.
- **Layouts**: full panel, compact one-line or FPS only. Pick a corner or drag it anywhere, and set the text size, background opacity and colors. Labels can follow your theme's accent color.
- **Per-game presets**: each game remembers its own overlay, crosshair and layout automatically. You can also edit a game's preset on the Overlay page.
- **Automatic**: detects the game in the foreground and shows the overlay only while a game is focused. Games in Steam, Epic, GOG, Xbox, EA, Ubisoft, Riot and other library folders are recognized right away, and anything else that renders like a game is picked up after a couple of seconds. You can mark any app *Always* or *Never* on the Games page.
- **Benchmarks**: press a shortcut to record a run and press it again to stop, or set it to stop after 30 s, 60 s, 2 min or 5 min. You get average FPS, 1% and 0.1% lows and a frame-time graph. Runs are saved per game, and you can compare any two side by side or export a run as CSV.
- **Crosshair**: dot, cross, circle, T or your own PNG, with size, gap, thickness, color, outline and opacity settings. It stays centered on the game window, and you turn it on or off per game.
- **Shortcuts** (all changeable): show or hide the overlay `Alt+Shift+O` · cycle layouts `Alt+Shift+L` · start or stop a benchmark `Alt+Shift+B` · crosshair on or off `Alt+Shift+X`
- **Themes**: the 14 suite themes (Dark with Mica by default, Light, Windows, Midnight, Glass, Frost, Twilight, Forest, Sunset, Ocean, Rose, Graphite, Paper, Mist), accent colors, and your own theme files in Hangar's format. They all switch live.
- **Tray and startup**: lives in the tray and can start with Windows (optionally with admin rights, no UAC prompt). `Ctrl+,` opens Settings.
- **Built-in updater**: checks GitHub Releases on launch and every few hours, on the Stable or Beta channel. Updates download in the background and are verified with SHA-256. Reticle never updates or asks you anything while a game is running, and if a new version doesn't start, it rolls back on its own.

## Safe with anti-cheat

Reticle **never injects into games and never hooks DirectX, Vulkan or OpenGL**. It doesn't even open the game's process memory. It only asks Windows which window is in front and what its exe is called.

- **Frame data** comes from **ETW** (Event Tracing for Windows), the same way Intel's PresentMon gets it. Windows' graphics stack already logs every present call (`Microsoft-Windows-DXGI`, `-D3D9` and `-DxgKrnl`), and Reticle listens to those events. It works for DirectX 9 through 12, Vulkan and OpenGL.
- **The overlay and crosshair** are separate transparent, click-through, always-on-top windows. Windows composes them over the game.
- There's no FPS limiter, because limiting frames needs injection.

## Permissions

| What | Needs | Why |
|---|---|---|
| FPS, frame times, lows, benchmarks | Admin rights | Windows only lets administrators start an ETW trace session. The first time Reticle starts without that permission (and no game is running), it asks **once** through Windows' UAC prompt. That sets Reticle to start with admin rights at sign-in through a scheduled task and adds your account to *Performance Log Users*. Every launch after that, including from Hangar or the Start menu, switches to the admin copy with no prompt. You can also do it from **Home → Allow**. |
| CPU temperature | Admin and the free [PawnIO](https://github.com/namazso/PawnIO.Setup) driver | This is how Ember and LibreHardwareMonitor read it. Reticle includes PawnIO's signed AMD (Zen) and Intel modules, unmodified. |
| GPU stats | An NVIDIA GPU and driver | They're read through NVML (`nvml.dll`, which comes with the driver). |
| Everything else | Nothing | |

Without permission the overlay still shows CPU, GPU, RAM and the rest (you can turn that off). Reticle never asks while a game is running. `reticle.log` (in the data folder) records whether frames arrived for each game, which helps if FPS ever shows a dash.

## Light on resources

- **No game, no work**: Reticle runs no timers while no game is in front. It waits for Windows to report a foreground change, and it stops the trace session 20 seconds after the last game leaves.
- **In game**: the overlay is drawn with Direct2D into a small bitmap 4 times a second (adjustable from 0.1 s to 1 s), and sensors are read once a second. That's a fraction of a millisecond each time, well under 1% of one core, with no GPU work in the game's process.
- **One ~0.9 MB exe**: no runtime, no services, no accounts, no telemetry. Reticle only connects to GitHub to check for updates. The Settings window is freed when you close it.

## Install

Download `Reticle.exe` from the [latest release](../../releases/latest) and run it. It runs from anywhere, or you can install it from **Hangar**. Windows may show SmartScreen the first time because the exe isn't signed yet: click **More info → Run anyway**.

**Portable mode**: put an empty file named `portable` next to `Reticle.exe`, and settings, benchmarks and themes go in a `data` folder beside it. Otherwise they're in `%LOCALAPPDATA%\Reticle`.

Command line: `--tray` starts quietly in the tray, and `--quit` closes the running copy.

## Themes

Reticle uses the suite's theme format (see [Hangar's THEMES.md](https://github.com/larosa023/Hangar/blob/main/docs/THEMES.md)). Go to **Themes → Open folder**, drop in a `.json` theme, and click **Reload**. **Save as file** writes the current theme as a starting point.

## Build from source

```bash
./build.sh                         # Linux/macOS, needs Zig: pip install ziglang
cmake -S . -B build -A x64 && cmake --build build --config Release   # Windows, VS 2022
```

## Releasing a new version

1. Bump `RETICLE_VERSION` in `src/common.h` (for example `0.1.0` → `0.1.1`) and update `CHANGELOG-latest.md`.
2. Commit and push to `main`.
3. The **Build** workflow compiles Reticle and publishes a GitHub Release tagged `v0.1.1` with `Reticle.exe`, a zip and `SHA256SUMS.txt`. Versions with a `-` (like `0.2.0-beta.1`) are published as pre-releases for the Beta channel.

Pushes that don't change the version don't create a release.

## Tech stack

| Piece | Used for |
|---|---|
| C++17, Win32, Direct2D/DirectWrite | The overlay, crosshair and Settings, in the suite's stack. Cross-compiled with [Zig](https://ziglang.org) |
| ETW (`StartTrace`/`ProcessTrace`) | Frame timing, as in [PresentMon](https://github.com/GameTechDev/PresentMon) |
| NVML | NVIDIA GPU usage, temperature, clock, power, VRAM |
| [PawnIO](https://github.com/namazso/PawnIO) | CPU temperature (AMD SMN / Intel MSR) |
| WinHTTP | Update checks (reads Hangar's release index first, then the GitHub API) |

## Project layout

```
src/
  main.cpp       single instance, command line
  app.cpp        tray, shortcuts, game detection, timers, benchmarks, updates
  frames.cpp     ETW frame capture and statistics
  sensors.cpp    CPU, GPU (NVML), RAM, CPU temperature (PawnIO)
  games.cpp      foreground window → game?
  overlay.cpp    overlay and crosshair windows
  bench.cpp      saved benchmark runs
  ui.cpp         Settings window
  updater.cpp    self-update with SHA-256 checks and rollback
  system.cpp     admin, Performance Log Users, start with Windows
  themes.cpp, gfx.cpp, json.h, net.cpp, util.cpp, config.cpp   shared with the suite
res/             icon, manifest, version info, PawnIO modules
```

## License

MIT. The bundled PawnIO modules (`res/pawnio`) are LGPL-2.1 and are used unmodified.
