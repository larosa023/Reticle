## Reticle 0.1.2

- **Fixed:** whole numbers on the overlay (FPS, 1% and 0.1% lows, CPU and GPU %, temperatures, GB totals) always showed **0**. Frame capture was working; the numbers were printed wrong. Every release is now checked with a screenshot of the overlay over a running test game.
- **Fixed:** ordinary apps were being treated as games. 0.1.1 counted any big window that kept drawing for a few seconds, so apps like video players and launchers got the overlay. Now a game has to be in a game library folder, or fill the screen (borderless/fullscreen) and keep rendering, or be one you marked as a game.
- Launchers (Minecraft Launcher and other apps with "Launcher" in the name) are no longer treated as games. The game they start still is.
- Apps that 0.1.0/0.1.1 wrongly saved as games are cleared once. Real games are found again the next time you play.
- The tray menu has "*App* isn't a game" for anything Reticle gets wrong.
- More video and music apps are ignored (Stream Tube, FreeTube, Plex, Jellyfin, Kodi, Netflix, Spotify-style players and others).
