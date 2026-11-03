![GexRecompiled logo](docs/GexRecompLogo.png)

**Disclaimer:** This project and its coordinator are not affiliated with Crystal Dynamics, Microsoft or Square Enix. Gex is a registered trademark of Crystal Dynamics and later Square Enix, and the rights to the PC version belong to Microsoft. The trademark is used only to indicate compatibility with their software.

## What is this?

A work-in-progress AI-led static recompilation of the [Windows release of Gex (1995) from GOG](https://www.gog.com/en/game/gex) for Linux, Windows, macOS, Android and Dreamcast, using [pcrecomp](https://github.com/sp00nznet/pcrecomp) by sp00nznet and a version of the [Cursor Agent skill](https://github.com/DohmBoy64Bit/pcrecomp-skill) by DohmBoy64Bit adapted for GitHub Copilot. Huge thanks to the creators of those repositories.

## Usage

On Windows and Linux, launch the release binary from a writable folder.
If the default `Gex` folder is missing, a folder picker asks for your original
GOG installation and copies it into `Gex`. Linux requires `zenity` or `kdialog`
for the picker. You can still supply an existing installation with `--data PATH`
to use it directly without copying.

On macOS, launch **Gex Recompiled.app**. It ships without original game files;
the first-run picker copies your installation into
`Gex Recompiled.app/Contents/Resources/Gex`. Move the app to a writable location
before importing, rather than launching it from a read-only disk image.
On Android, the system folder picker imports into private app storage.
Before copying, imports verify the presence, exact size and SHA-256 checksum
of all **661 required files** from the supported GOG installation, including
the executable, movies, levels, music, sound effects and visual assets.
The copied files are checked again before the import is accepted.
Missing, damaged or modified required files are reported by name. Imports leave the
original installation untouched, and never overwrite an existing destination.
Canceling the picker closes setup without starting the game.
Before selection, setup displays: "Select the folder with the installation of
the PC version of Gex from GOG." Desktop pickers are titled "Select the Gex
installation folder"; Android receives the same requested title, but its
system document picker may retain a system-defined header.

SDL desktop and Android builds use **bgfx by default**. Use
`--renderer software` to select the CPU renderer when included; Dreamcast
continues to default to PVR.

The ARM64 and Intel macOS apps require macOS 13 or newer and include their
runtime libraries; bgfx normally selects Metal.
Distributable `gex-recomp-macos.zip` archives always exclude imported game data.
See the [development guide](docs/development.md#macos-arm64-and-intel)
for cross-building and the extent of headless Mac validation.

The experimental [Android APK](docs/development.md#android) requires Android 8+
and OpenGL ES 3.1. Vulkan and OpenGL ES are bundled in the same build through
bgfx. Supply your own game files separately and use a physical controller or
keyboard; there are no on-screen controls.

### Password saves

The SDL desktop builds (Linux, macOS and MinGW Windows) and Android have **three
Video Deck save slots**. Starting a game requires choosing a deck in the
screenshot-derived menu.
**PLAY** restores an occupied deck or starts a new game in an empty one.
Passwords earned at the original password TVs are saved automatically to that
deck. These are progression checkpoints, not position/health save states.

Use arrows/D-pad to select and **X / gamepad A** to confirm, **C / gamepad B**
to cancel. **ERASE** and overwriting a **COPY** destination require confirmation,
defaulting to cancel. **V / gamepad Y or shoulder** opens the original manual
password entry for the selected deck. The menu also accepts mouse/touch input;
this does not add touch controls to gameplay.

Saves live in `passwords.sav` beside the per-user `settings.reg`, never in the
original game installation. Dreamcast/VMU and the legacy MSVC native32 host
retain their original password behavior for now.

### Options

On SDL desktop and Android, the title screen's **Options** entry replaces
**Password** without changing the other title entries or their layout. The
original-style screen provides Sound, Voice and Music switches, a Music Test,
action assignments and Exit. Left/right or confirm changes the selected value;
back or Exit saves changes to the existing per-user settings.

Controls cycle the four action keys (C, Z, X and V) and their existing gamepad
equivalents, swapping occupied bindings so all actions remain available.
Tongue Lash and Hurl share a binding. Menu navigation and confirmation stay
fixed even after gameplay bindings change. Music Test previews installed tracks
and restores the title music on exit.

### Launch options

| Option | Description |
|--------|-------------|
| `--help` | Self-explanatory. |
| `--data PATH` | Use an existing game installation directly, without copying or opening setup. Defaults to `Gex` in the current working directory, or `Contents/Resources/Gex` inside a macOS app bundle. |
| `--render-size WIDTHxHEIGHT` | Set the gameplay resolution. `427x240` provides widescreen; higher resolutions are not recommended. |
| `--skip-movies` | Skip the FMVs on startup. |
| `--renderer NAME` | Select `software`, `opengl`, `pvr`, `opengl12` or `bgfx`, if included in the build. `pvr` is Dreamcast-specific; `opengl12` is the experimental fixed-function OpenGL/GLdc renderer; `bgfx` is the experimental portable backend. |
| `--hardware-renderer` | Select bgfx when included, or the platform's other GPU renderer. Dreamcast selects PVR. |
| `--check` | Validate the original executable and initialise the recompilation without starting the game. |
Windows native32 launcher supports only `--help`,
`--data` and `--render-size`.
Headless, `--check`, and bounded smoke runs never open the desktop picker.

## Why GOG? Can I use another version?

The GOG version was chosen due to accessibility. The physical PC release is untested; if you have it, I would greatly appreciate it if you could test it and report whether it works.
The 5th-gen console or Gex Trilogy releases are unsupported.

## Should I actually use this?

Not right now, as there's a substantial amount of work left to make this a worthwhile version of the game.

## AI-led?

As in entirely vibecoded and only vaguely steered due to a lack of necessary knowledge and the output being adequate. You may have concerns regarding this, but I've made my choice. However, if you do have the necessary knowledge to produce a human-led project with the same goal, I urge you to proceed, as this would be the better outcome for the preservation of Gex.

## Features

- Native OpenGL rendering, bypassing the original software rasteriser and DirectDraw presentation.
- Experimental bgfx rendering, including Metal on Apple Silicon.
- Native OpenAL and DirectSound backends for audio.
- Custom resolution support (widescreen 427x240 works fine minus the occasional odd texture tiling; usage of higher resolutions is not recommended).
- Three password-backed autosave decks on SDL desktop and Android.

## Controls

Remapping is not implemented yet.

Keyboard:

Movement - arrow keys
Jump - X
Tail whip - Z
Tongue - C
Run - V
Fullscreen - F11

SDL Gamepads:

Movement - D-pad/left joystick
Jump - A
Tail whip - X
Tongue - B
Run - Right shoulder button

## Planned features

- In-game settings
- Remapping
- Texture pack support
- Dreamcast VMU storage for password-backed save decks
- UWP (Xbox One/Series) and PSP support (feel free to contact if you're able to test on PSP or Xbox One)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).
