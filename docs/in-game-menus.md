# In-game menus: structure and editing guide

This guide maps the title, Video Decks and Options workflows to the code
that implements them. It is intended for contributors, including agents with
limited context. Check the current source before editing: labels, row indices
and guest addresses are implementation details, not a stable extension API.
For renderer internals see [renderer debugging](renderer-debugging.md); for
instruction-boundary hooks see [generated-code changes](generated-code-changes.md).

## What the player sees

On SDL desktop and Android, the retail title screen retains its Start and Exit
entries; the original Password label is replaced with **Options**. Start opens
the native **Video Decks** menu. Options is a separate title-screen modal, not
the retail in-level pause menu. Android's folder picker and setup launcher are
also separate from these in-game screens.

```text
Title
├── Start → Video Decks
│   ├── Deck 1 / 2 / 3: PLAY, ERASE, COPY
│   ├── Manual password entry for the selected deck
│   └── EXIT / Back → Title
├── Options
│   ├── Controls → Gameplay and Menus bindings → Exit / Back
│   ├── Audio → Sound, Voice, Music, Music Test → Exit / Back
│   ├── Video → Render backend, Display, Render resolution, Vsync → Exit / Back
│   └── Exit / Back → Title
└── Exit → retail exit flow
```

In Video Decks, PLAY selects the autosave target and either starts a new game
or restores the stored password using retail code. ERASE and overwriting a
COPY destination require confirmation; manual-entry replacement of an occupied
deck does too. Cancellation is the default. A valid manual password and
passwords earned at TVs update the selected deck. This is password progress,
not an arbitrary position or health snapshot.

Controls has separate keyboard and gamepad columns; directions select a row,
left/right selects a column, and confirm starts capture. Audio volumes are
0–100 in five-point key steps or adjustable with a pointer/drag. Music Test
changes the requested track. Video offers the compiled renderer/API choices;
backend and API changes require restart, display changes are immediate, and
resolution is applied when leaving Options. Vsync stays on; the row toggles
triple buffering only on supported active bgfx APIs. Availability depends on
the platform and renderer.

The title hook and native menus are guarded by `GEX_ENABLE_PASSWORD_SAVES`,
enabled by `CMakeLists.txt` for the desktop-test/HLE platform configuration.
Dreamcast and the legacy MSVC native32 host use the retail title/password flow;
they do not acquire native Options merely because a function has a platform
enum. Dreamcast VMU save persistence is not implemented. See the platform
manifests for the actual build's selection.

## Code ownership and flow

| Concern | Starting point | Responsibility |
| --- | --- | --- |
| Entry from retail title | `tools/save_rewrite.py`, `tools/generate.py` | Verified guest-address hooks, title-label initialization, deck selection, password checkpoints, scoped text drawing and close handling. |
| Modal lifetime, decks, transitions | `src/save_menu.c`, `src/save_menu.h` | `run_menu`, input filters, fade states, deck actions and the retained title frame. |
| Options pages | `src/options_menu.c`, `src/options_menu.h` | Page/selection state, layout, drawing, keyboard/pointer handling, capture, settings load/save and video effects. |
| Controls and defaults | `src/preferences.c`, `src/preferences.h` | Actions, bindings, defaults, conflict checks, canonical guest keys, renderer/API preferences. |
| Input routing | `src/main.c`, `src/gamepad.c`, `src/platform/desktop/input.c`, `src/platform/input.h` | Menu-context detection, physical input and capture filters, button/key release handling and platform events. |
| Save data | `src/save_store.c`, `src/save_store.h`, `src/platform/filesystem.h` | Three validated password slots and replacement writes, independent of the menu artwork. |
| Drawing and screen transforms | `src/graphics_guest.c`, `src/renderer.c`, `src/platform/*/` | Guest packet drawing, clipping, fades, resize and software/GPU presentation. |
| Cabinet art | `assets/save-menu.bin`, `tools/save_menu_assets.py`, generated `save_menu_art.h` | Embedded 320×240 Video Decks image and selection patches, not Options art. |
| Behavior and visual tests | `tests/password_saves.*`, `tests/options_screens.*`, `tests/save_handoff.*`, `tests/test_save_rewrite.py` | Store/input and modal cases, rendered screens, title handoff, rewrite boundaries. |

`src/main.c` loads preferences and initializes the save menu after guest boot,
then installs menu-context detection. `gex_options_title_init()` changes the
retail title label, not its widget. The injected title dispatcher calls
`gex_save_choose()` for Start or `gex_save_options()` for Options.
`gex_save_choose()` returns 0 for cancel, 1 for PLAY, and 2 for retail manual
password entry. `gex_save_options()` wraps `gex_options_begin()`, `run_menu()`
and `gex_options_end()`.

The guest worker enters the modal while the host continues pumping events.
The wait uses `hle_block_begin/end()` so other machine-owning work can run;
do not replace it with a busy loop or manipulate guest state outside the
existing synchronization. The modal moves through title-out, menu-in, idle,
menu-out and title-in states, with 250 ms fades. Input is suppressed during
transitions; held inputs are released/blocked so the opening press does not
activate the next screen. PLAY holds the outgoing image black until the
separate graphics rewrite releases it at the retail transition. Window close
uses a one-shot request and the scoped quit hook; it is not the same as Back.

## Options implementation

`page` is `CATEGORIES`, `CONTROLS`, `AUDIO`, or `VIDEO`. `selection` is a
zero-based row index within that page. Categories use the order Controls,
Audio, Video, Exit; the `page = selection + 1` mapping and `page - 1` return
selection depend on this order. The last row of every page is Exit.

| Page | Labels/rows | State and action routing |
| --- | --- | --- |
| Categories | `headings[]`, `entry_count()` | `activate()` enters the chosen page or flushes/exits. |
| Controls | `control_labels[]`, `GEX_ACTION_COUNT` | First eight rows correspond to `enum gex_action`; `binding_column` and capture filters choose keyboard/gamepad. Ninth row exits. |
| Audio | `audio_labels[]`, `tracks[]` | Rows 0–2 are `gex_prefs.volume[]`; row 3 is Music Test; row 4 exits. `volume()` updates guest values and audio setters. |
| Video | `video_labels[]`, `backends[]`, `resolutions[]` | Rows 0–3 select renderer/API, display, resolution and buffering; row 4 exits. Choices are collected in `gex_options_video_init()`. |

The counts, indices and labels are currently handwritten in multiple places,
not a data-driven menu framework. `entry_top()`, `entry_height()`,
`entry_y()`, `follow_selection()` and `viewport_bottom()` control layout and
scrolling. `gex_options_key()` and `gex_options_pointer_event()` converge on
`activate()`; pointer hit boxes use the displayed scroll position. A drag
remains attached to the chosen slider until release. Keep keyboard, controller
and pointer behavior consistent.

Options draws the retail animated TV static, green title font and HUD hand
through guest routines. `text()` uses a temporary guest-stack copy of the
title object; `load_hand()` loads and later releases a private HUD resource.
`gex_graphics_menu_clip_begin/end()` clips scrolling rows and the hand;
slider rectangles use the same viewport. The heading and footer remain fixed.
The coordinate conversion between the 320×240 menu layout and 320×224
authored picture matters for drawing, hit testing and clipping. Do not change
only the visual position without updating the corresponding interaction.
Video Decks instead composites its embedded artwork into a 320×240 RGB565
buffer and presents it through the CPU or GPU-owned path.

## Settings, input and persistence

`gex_preferences_reset()` supplies defaults. `gex_options_load()` reads
versioned DWORDs under `Software\\GexRecomp\\Options`, or migrates retail audio
switches and key bindings when the version is absent. `save()` writes the
version, bindings, volumes and video values via the HLE registry and flushes
`settings.reg`. If a saved renderer is not compiled into the current build, the
loader selects the first available renderer and resets its API to Auto while
preserving the other saved options. Exit/Back from categories flushes dirty
settings; a failed flush displays an error and normally leaves the menu open
for retry. The window-close path attempts a flush and exits rather than
returning to the title. `--renderer` and `--render-size` override saved
selections at startup.

`gex_controls_bind()` validates allowed inputs and conflicts within gameplay
or menu context; Tongue Lash and Hurl intentionally share a canonical guest
destination. Arrow movement and emergency menu keys remain available.
Keyboard and gamepad capture filters consume the physical event and its
release; context changes block held pad buttons. Preserve these rules when
adding actions or changing capture.

`passwords.sav` lives beside `settings.reg` on supported hosts. The fixed
39-byte `GEXDECK1` format holds three nine-byte password slots and a CRC32.
`gex_save_store_open()` validates the full record and retail password bounds;
write/copy/erase use replacement writes and leave the old state intact on
failure. Do not use the options registry as a save-slot store, silently repair
invalid data, or infer VMU support from the shared interface.

## Organized edit recipes

1. **Identify the screen and ownership.** Trace the player entry point and
   state through the table above. Determine whether the request concerns the
   retail title/pause menu, Video Decks, or native Options. Preserve the
   existing platform gate and unrelated menus.
2. **Define behavior before layout.** Specify the value, default, allowed
   range, input actions, platform/backend availability, application timing,
   persistence and failure behavior. Keep the last Exit row and all page/row
   mappings coherent. Avoid relying on numeric indices without updating all
   their users.
3. **Change all affected paths together.**
   - For an Options row, update its label, `entry_count()`, layout and
     scrolling, `activate()`, `gex_options_render()`, pointer hit testing and
     any row-specific handling. For example, inserting an Audio row before
     Music Test changes the `selection < 3` and track/Exit assumptions; search
     the page's conditions rather than changing just its label.
   - For a persisted preference, update the type/default in `preferences.h`
     and `preferences.c`, its load validation and save path, application
     timing, and error/retry behavior. Plan compatibility for existing
     version-1 records: the present loader requires its named fields. Do not
     silently change the meaning of an existing key or introduce a missing
     required field without a migration/version strategy.
   - For an action binding, update `enum gex_action`, default bindings,
     destination mapping, Controls labels/counts and gameplay/menu grouping,
     conflict validation, `KeyN`/`PadN` persistence and input tests. Inserting
     into the enum changes persisted indices and needs migration.
   - For a Video Deck action, update selection and confirmation state,
     `gex_save_key()`, pointer hit boxes, `activate()`, rendering, store
     operation and failure/cancel cases. If changing cabinet artwork, inspect
     the asset tool and its inputs; do not hand-edit `save_menu_art.h`.
   - For a new guest hook or retail transition, change the checked-in rewrite
     and handwritten callback, preserve guest registers/flags and
     nonmatching control flow, and follow [generated-code changes](generated-code-changes.md).
     Never hand-edit `src/recomp/gen/`; ordinary menu C edits do not require
     regeneration.
4. **Exercise boundaries, not only the happy path.** Cover first/last rows,
   wrapping, scrolling and clipping, pointer coordinates after scrolling,
   capture cancellation and held input, page Back/Exit, fade/close behavior,
   save failure/retry and reload as relevant. For rendering changes check both
   CPU and GPU-owned presentation where supported and report what was
   actually observed.

## Focused validation

Use the [testing skill](../.github/skills/testing/SKILL.md) for headless,
silent, isolated runs and the [build troubleshooting guide](build-test-troubleshooting.md)
for configuration and failure handling. Build the appropriate targets before
CTest, list the expected tests first, and do not treat zero matches as a pass.
Example for an existing configured Linux build:

```sh
cmake --build build/linux/release --target gex_password_saves_test gex_options_screens_test gex_save_handoff_test --parallel 3
ctest --test-dir build/linux/release -N -R '^(password-decks|options-screens|save-handoff)$'
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ALSOFT_DRIVERS=null \
  ctest --test-dir build/linux/release --output-on-failure -R '^(password-decks|options-screens|save-handoff)$'
```

`password-decks` tests store integrity, menu logic, bindings and preference
reload/failure using isolated directories. `options-screens` exercises the
real title callback, categories, scrolling, sliders, screenshots and
resolution/reload on the backends configured by CMake. `save-handoff` checks
title-to-deck transitions. They do not prove a full playthrough or device
behavior; inspect their actual configured backend list. For hook changes also
run:

```sh
.venv/bin/python -m unittest discover -s tests -p 'test_save_rewrite.py'
```

Run only checks relevant to the change; missing dependencies, a failed link,
or an unavailable target must be reported, not counted as a pass. Do not
cross-build or launch a device/emulator for routine Linux validation without
the user's explicit request. Preserve the original `Gex/` installation and
user settings.
