# inconsole-runtime

`inconsole-runtime` is a lightweight launcher and runtime shell for InConsole devices. It runs as a kiosk interface on `tty0`, renders UI via SDL, and provides core scenes such as launcher, first-run setup, settings, diagnostics, system info, file manager, and power-off.

## Key Features
- Kiosk UI on `tty0` with scene management and transition effects.
- Built-in settings for brightness, volume, language, theme, and animations.
- Diagnostics with battery status reporting.
- File manager and system information views.
- Application discovery in `/userdata/apps` using `app.json` manifests.
- Optional input bridge `inconsole-input-bridge` for mapping controller input to keyboard events (e.g., DOOM).

## Repository Layout
- `src/` – C++ runtime sources and the input bridge.
- `assets/` – system assets (e.g., runtime icons).
- `userdata/` – default user data, apps, and language packs.
- `inconsole-runtime.mk`, `Config.in` – Buildroot package integration.

## Buildroot Integration
This project is provided as a local Buildroot package.

Dependencies:
- `sdl`
- `freetype`
- `libpng`
- `dejavu`

Enable the package:
- `BR2_PACKAGE_INCONSOLE_RUNTIME`

Buildroot produces two binaries:
- `inconsole-runtime`
- `inconsole-input-bridge`

and installs data into:
- `/usr/share/inconsole/system-icons`
- `/usr/share/inconsole/default-apps`
- `/userdata` (copied from `userdata/` in the repository)

## Data Root and Configuration
The default data root is `/userdata` and can be overridden via `INCONSOLE_DATA_ROOT`.

Key files:
- `/userdata/system/settings.json` – user settings (language, brightness, etc.).
- `/userdata/system/profile.json` – profile and PIN configuration.
- `/userdata/system/languages/*.json` – UI translations.
- `/userdata/system/logs/runtime-core.log` – critical runtime logs.
- `/userdata/system/postscript.sh` and `undo_postscript.sh` – scripts used during first-run and reset flows.
- `/userdata/system/reset-remove.list` – paths removed during factory reset.

## Applications (`/userdata/apps`)
Each application has its own directory with an `app.json` manifest. Example: `userdata/apps/doom/`.

Common fields:
- `id`, `name`, `description` – UI metadata (supports `PL|EN` variants).
- `icon` – app-local icon path.
- `exec`, `args` – launch command and arguments.
- `category`, `order` – launcher categorization and ordering.
- `exclusive_runtime` – runs the app in exclusive mode to free launcher memory.

## Environment Variables
- `INCONSOLE_DATA_ROOT` – data root (`/userdata`).
- `INCONSOLE_FORCE_FIRST_LOGIN` – forces first-run scene.
- `INCONSOLE_TARGET_FPS` – target FPS for the render loop.
- `INCONSOLE_SCENE_FADE_MS` – scene fade duration (60–600 ms).
- `INCONSOLE_SCENE_FADE_ALPHA` – scene fade intensity (40–220).
- `INCONSOLE_LAYOUT_GUARD` – enables additional layout validation.
- `INCONSOLE_SYSTEM_ICONS` – system icons directory (default `/usr/share/inconsole/system-icons`).
- `INCONSOLE_FULLSCREEN` – forces SDL fullscreen.
- `INCONSOLE_FORCE_SOFTWARE_RENDERER` / `INCONSOLE_LOW_MEM` – low-memory rendering mode.
- `INCONSOLE_CACHE_TEXT_MB`, `INCONSOLE_CACHE_ICON_MB`, `INCONSOLE_CACHE_ICON_SCALED_MB`, `INCONSOLE_CACHE_STYLE_MB` – cache budgets.
- `INCONSOLE_CACHE_WIDTH_ITEMS`, `INCONSOLE_CACHE_DEBUG` – text width cache tuning.

## Diagnostics and Recovery
On fatal errors, the runtime writes a log to:
`/userdata/system/logs/runtime-core.log`

and switches to recovery mode with a message on `tty0`.

## inconsole-input-bridge
Creates a virtual keyboard via `uinput` and maps `evdev` controller input (D-Pad/axes/buttons) to keyboard keys. Useful for applications that require keyboard input.
