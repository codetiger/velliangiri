# Velliangiri 1280x720 TV Shell (`/bin/tvshell`)

Native ten-foot television shell for the S5L8947X (`j33iap`) implementing the design system in `tools/tvshell/design/tokens.json`.

## Key Features

- **Seamless `1280x720@30` (CEA VIC 62) Display Pipeline:**
  - Renders directly to a cached `1280x720` `XRGB8888` backbuffer and copies only damaged rectangles to `/dev/dri/card0` (or `/dev/fb0`).
  - Bypasses the display-pipe gamma LUT (`gamma_brightness = 100`) so sRGB design tokens are emitted bit-exact over HDMI.
  - Releases DRM master cleanly before launching external applications (such as `/bin/doom`) and restores `1280x720@30` when they exit.
- **Two Full-Parity Themes (`Paper` & `Signal`):**
  - **Paper:** Warm daylight editorial palette (`#F4EFE6` ground, `#141312` ink, `#C84B31` terracotta focus rule, **Literata** + **Atkinson Hyperlegible**).
  - **Signal:** Dark room high-contrast palette (`#0E1012` ground, `#F2EFE9` ink, `#FFB000` amber focus rule, **Atkinson Hyperlegible**).
  - **First-Start Theme Picker:** Shown automatically on first boot when `/mnt/data/linux/etc/shell.conf` has no saved theme, and reachable at any time from the options list.
- **Predefined Turns & Interactive Specimens (Phase 1):**
  1. **Home / Applications & Specimens:** Launch **Doom (`S5L8947X`)**, open the **Media Gallery**, inspect **Live System Status**, open the **Sourdough Starter** recipe & working-state specimen, browse **Notes & Folders**, or switch between **Paper** and **Signal** themes.
  2. **Doom (`S5L8947X` Native Port):** Launch into the title screen, warp directly to **E1M1 (`Knee-Deep in the Dead`)**, launch in **16:9 Widescreen**, or return to Home.
  3. **Live System Status (`64 px` Value):** Real-time SoC temperature, RAM usage (`/proc/meminfo`), `/mnt/data/linux` free space (`statvfs`), and system uptime.
  4. **Media Tiles (`4 x 248x140`) & Full-Screen Viewer (`1280x720`):** Four media tiles with duration badges and `3 px` bottom focus bar; pressing `Select` opens the full-screen `1280x720` viewer with a `2000 ms` auto-hiding translucent metadata chip (`Play/Pause` toggles the chip; `Left/Right` steps items; `Menu` closes).
  5. **Sourdough Starter Specimen & Working State:** Exercises `Answer` (`28 px`), numbered steps (`22 px`), `Sources` (`16 px`), and the animated **Working** state (`500 ms` logo mark blink + `6 px` Signal progress bar).
  6. **Notes & Persistent Storage:** Lists files and directories under `/mnt/data/linux/`.
- **7-Button IR Remote + Bluetooth Keyboard (`Ask` Line):**
  - Automatically detects Bluetooth/USB keyboards via `inotify` on `/dev/input` + `EVIOCGBIT` and reveals the `44 px` **Ask** input bar when a keyboard is present.
  - Supervises external applications and terminates them cleanly on a **2-second `Menu` hold** (`menuHoldMs: 2000`).

## Building & Installing

```sh
./tools/tvshell/fetch-fonts.sh    # Download Literata + Atkinson Hyperlegible TTFs
./tools/tvshell/build.sh          # Generate token headers & build build/tvshell/tvshell
./tools/tvshell/install.sh [HOST] # Install /bin/tvshell, /bin/bt, and fonts over SSH
```
