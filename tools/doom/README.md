# Doom for S5L8947X (`j33iap`)

Native ARMv7 NEON port of `doomgeneric` (`GPL-2.0-only`) targeting the S5L8947X display, audio, and input hardware without X11, Wayland, SDL, or `alsa-lib` dependencies.

## Hardware Integration

- **Display (`1280x720@30` `XRGB8888`, Native `960x600` / `1066x600` 3D Renderer):**
  - `tools/doom/patch-hires.py` upgrades Doom's software 3D renderer (`r_*`) to render walls, floors/ceilings, skies, sprites, and weapons natively at **`960x600`** (`3x` the original `320x200` resolution) into `I_HiresBuffer`, while compositing 2D menus, HUD text, and the status bar in `320x200` space via `I_UiOverlayMask`.
  - **Default (`4:3` aspect ratio):** Native `960x600` (`3x3` integer scale) centered at `(160, 60)` inside the `1280x720` CEA-861 Title Safe area (`60 px` top/bottom and `160 px` left/right margins).
  - **Widescreen (`-widescreen`):** Native `1066x600` (`16:9` with true `96°` horizontal field-of-view expansion and zero horizontal stretching) centered at `(107, 60)`.
- **Audio (`/dev/snd/pcmC0D0p`):** Drives the kernel ALSA PCM interface directly via `SNDRV_PCM_IOCTL_HW_PARAMS` / `SNDRV_PCM_IOCTL_WRITEI_FRAMES` at `48000 Hz` 16-bit stereo (`S16_LE`) in a dedicated mixer thread:
  - **16-channel digital SFX mixer:** Decodes WAD `DS*` DMX PCM sound lumps with 32.32 fixed-point resampling and stereo separation.
  - **Polyphonic `MUS` score synthesizer:** Parses `MUS\x1a` music lumps at `140 Hz` and synthesizes polyphonic voices + channel 15 percussion.
- **Input (`/dev/input/event*`):**
  - Automatically toggles `/sys/bus/serdev/devices/serial0-0/game_map` to `1` on launch and restores `0` on exit.
  - Hot-rescans `/dev/input` so Bluetooth keyboards paired before or during gameplay work alongside the 7-button IR remote.

## Controls

### 7-Button IR Remote

| Button | In Menus / Title / Intermission | In Level Gameplay |
| :--- | :--- | :--- |
| **Up / Down** | Navigate menu items | Move forward / backward |
| **Left / Right** | Adjust sliders | Turn left / right |
| **Select (Center)** | `Enter` (confirm menu / `'y'` on prompts) | **Fire weapon** (or respawn when dead) |
| **Play / Pause** | **Use / Open** | **Open doors / switches** (double-tap cycles weapons) |
| **Menu (short press)** | Back / Close menu (`'n'` on prompts) | Open Doom menu (`Esc`) |
| **Menu (hold 2 s)** | **Exit Doom back to `tvshell`** | **Exit Doom back to `tvshell`** |

### Bluetooth / USB Keyboard

- **Move / Strafe:** `W A S D` or Arrow keys (`, ` and `.` also strafe)
- **Fire:** `Ctrl`
- **Use / Open door:** `Space` or `E`
- **Run:** `Shift`
- **Weapons:** `1`–`7`
- **Automap:** `Tab`
- **Toggle 4:3 / 16:9 Widescreen:** `F11`
- **Exit to `tvshell`:** Hold `Menu` / `Home` for 2 seconds, or choose **Quit Game** from the `Esc` menu.

## Building & Installing

```sh
./tools/doom/fetch.sh          # Download doomgeneric + Doom v1.9 Shareware (doom1.wad) + Freedoom Phase 1
./tools/doom/build.sh          # Apply patch-hires.py and build static ARMv7 binary -> build/doom/doom
./tools/doom/install.sh [HOST] # Install /bin/doom + doom1.wad + freedoom1.wad over SSH
```

## WAD & Savegame Paths

At startup, `/bin/doom` searches for an IWAD in the following order unless `-iwad <path>` is specified:
1. `/mnt/data/linux/games/doom/{doom1.wad,doom.wad,doom2.wad,plutonia.wad,tnt.wad,freedoom1.wad,freedoom2.wad}`
2. `/usr/share/games/doom/{doom1.wad,doom.wad,doom2.wad,freedoom1.wad,freedoom2.wad}`

Configuration (`.default.cfg`) and savegames (`.savegame/`) are stored persistently in `/mnt/data/linux/games/doom/`.
