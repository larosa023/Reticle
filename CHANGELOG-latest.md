## Reticle 0.1.0

The first release: a lightweight in-game performance overlay and crosshair, without injection.

- **Overlay**: FPS, frame time with a live graph, 1% and 0.1% lows, CPU usage and temperature, GPU usage, temperature, clock, power and VRAM (NVIDIA), RAM, clock and session length. Each one can be turned on or off.
- **Layouts**: full panel, compact line or FPS only. Pick a corner or drag it anywhere, and set the text size, opacity and colors. Each game remembers its own preset.
- **Automatic**: shows only while a game is in front and hides when you alt-tab.
- **Benchmarks**: record a run with a shortcut, then see average FPS, 1% and 0.1% lows and a frame-time graph. Runs are saved per game, and you can compare two of them or export a CSV.
- **Crosshair**: dot, cross, circle, T or your own PNG, with size, gap, thickness, color, outline and opacity settings. You turn it on or off per game.
- **Shortcuts**: Alt+Shift+O shows or hides the overlay, Alt+Shift+L cycles layouts, Alt+Shift+B starts or stops a benchmark, and Alt+Shift+X turns the crosshair on or off. You can change them all.
- **Safe with anti-cheat**: frame data comes from Windows' ETW graphics events, the way PresentMon gets it. Reticle never touches the game process.
- The 14 suite themes, accent colors and Hangar theme files. Tray icon, start with Windows, portable mode and a built-in updater with rollback.

Works over borderless and windowed games. Exclusive fullscreen hides any overlay window.
