# Giri on Velliangiri OS — 1280×720 TV Shell (`/bin/tvshell`, Schema v1.2)

Native 10-foot television shell for the S5L8947X (`j33iap`) implementing the **Giri on Velliangiri OS — Standardized Fixed-Length 1:1 TV Design System & Schema v1.2** (`tools/tvshell/design/tokens.json` and `screen.schema.json`).

## Key Architecture

- **32-bpp True-Color (`XRGB8888`) Framebuffer & DRM Pipeline:**
  - Renders into a double-buffered `1280×720` `XRGB8888` (`depth=24, bpp=32`) surface and presents damaged rectangles to `/dev/dri/card0` (or `/dev/fb0`).
  - Bypasses the display-pipe hardware gamma LUT (`gamma_brightness = 100`) and evaluates SVG `objectBoundingBox` gradients and `sRGB` facet stops accurately so the 3D faceted Velliangiri Silver Mountain logo renders without color loss or posterization on TV.
  - Releases DRM master cleanly before launching external applications (such as `/bin/doom`) and restores `1280×720` scanout when they return.
- **4-Window Composited UI System:**
  1. **Window 0 (`GIRI_WIN_MAIN`) — Clean 4-Slot Fixed-Length TV Screen (`1280×720`):**
     - **Slot 1 Header (`44px`):** `49×40` 3D faceted Velliangiri Silver Mountain logo (with 5-frame `65ms` ping-pong animation `[0,1,2,3,4,3,2,1]`), `"Giri"`, `"—"`, and `prompt_title`.
     - **Slot 2 Summary Box (`148px`):** Clean editorial summary card (`title` ≤64c, `source_caption` ≤48c, `text` ≤250c / 3 lines) with Stage-1 shimmer skeleton bars and sentence-level read-aloud highlighting (`Play/Pause` button).
     - **Slot 3 Media Cards Row (`312px`):** 4 selectable full-bleed `16:9` visual cards (`274.5×312px`, `rx=14`) with top badges, center video play circle, bottom gradient scrim, and `-4px` focus lift + amber focus ring.
     - **Slot 4 Bottom Bar (`58px`):** 4 selectable buttons (`188px 364px 364px 188px`): fixed bottom-left **Ask Giri**, 2 contextual follow-up buttons, and fixed bottom-right **Settings**.
  2. **Window 1 (`GIRI_WIN_MEDIA_OVERLAY`) — Fullscreen `1280×720` Media / Detail Viewer:**
     - Edge-to-edge 720p visual canvas with top gradient bar (`PHOTO VIEW` / `VIDEO STREAM` / `APP RUNNING` badge + `Menu / Esc = Back` pill), bottom gradient scrim, video `.player-bar`, and narrative overlay.
  3. **Window 2 (`GIRI_WIN_ASK_MODAL`) — Ask Giri Prompt & Suggested Topics Modal:**
     - Centered `920×512` modal over dimmed backdrop with text input field, `"Ask Giri"` submit button, 7 suggested topic chips, and `"Close"` / `"Done ↵"` row.
  4. **Window 3 (`GIRI_WIN_SETTINGS_MODAL`) — Velliangiri OS Settings Modal:**
     - Centered `960×636` modal with 5-column read-only system status strip (`Temperature`, `Storage`, `Memory`, `Network`, `Display`), 3-column OS Preferences (`Display Theme: Paper/Signal`, `Auto Read Aloud`, `Navigation Sound`), and 2×3 Supported AI Models grid loaded from on-device `supported_models.json`.
- **On-Device JSON Catalogs:**
  - Reads `screen.json`, `supported_models.json`, and `apps.json` from `/etc/tvshell/` or `/usr/share/tvshell/` (with compiled-in fallbacks) for standalone offline operation.

## Building & Installing

```sh
./tools/tvshell/fetch-fonts.sh    # Download Plus Jakarta Sans + JetBrains Mono TTFs
./tools/tvshell/build.sh          # Generate token/SVG headers & cross-compile build/tvshell/tvshell
./tools/tvshell/install.sh [HOST] # Install /bin/tvshell, fonts, and JSON catalogs over SSH
```
