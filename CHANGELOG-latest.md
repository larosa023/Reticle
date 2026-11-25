## Reticle 0.1.1

**FPS now works reliably in the overlay.**

- **Fixed:** FPS stopped showing after you alt-tabbed out of a game for more than 20 seconds and came back. Frame capture now restarts every time a game comes to the front.
- **Fixed:** without admin rights the overlay only showed a dash for FPS. Reticle now sets this up with a single Windows prompt (never while a game is running): it starts with admin rights at sign-in from then on, and any later launch, from Hangar or elsewhere, switches to the admin copy without another prompt.
- Reticle now checks whether Windows actually allows frame capture, rather than just whether it's running as admin. A restricted admin token can't capture.
- Games that render from a helper process in their own folder are followed automatically.
- If an admin copy is already running, opening Reticle again goes to that copy instead of starting a second one.
- `reticle.log` now records, per game, whether frames arrived, which makes "no FPS" reports easy to diagnose.
- Every release is now tested on a real Windows machine: frame capture for D3D11 and D3D9, detecting a game and getting its FPS, alt-tabbing out and back in, and the switch to the admin copy.
