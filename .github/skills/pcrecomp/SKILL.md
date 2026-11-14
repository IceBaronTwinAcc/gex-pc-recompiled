---
name: pcrecomp
description: "Gex Recompiled statically translates the supported original Windows x86 Gex executable into C. Use for guest startup, dispatch, callbacks, generated-code changes, rewrites, shims, and debugging with sp00nznet/pcrecomp."
license: MIT
---

# Gex static recompilation

Gex Recompiled statically translates the supported original Windows x86 Gex
executable into C, builds the translated functions for a native host, and
provides a compatibility runtime and platform services. The original executable
and game assets are still required as data; this is not a full source
decompilation. The normal HLE launcher explicitly reports that original x86 is
not executed. A Dreamcast emulator such as Flycast runs the **compiled SH4
host**, not the original PC instructions.

Read [project-structure.md](../../../docs/project-structure.md) for navigation,
[generated-code changes](../../../docs/generated-code-changes.md) for changes
to the lift, and [debugging](../../../docs/debugging.md) for investigations.

## What is owned where

| Component | Role |
| --- | --- |
| `Gex/GEX.exe` and the separate `pcrecomp/` checkout | Inputs validated by `tools/generate.py`; the original executable is user-owned. |
| `tools/generate.py` | Runs the lifter, calls rewrites, validates hooks and writes generated output. |
| `tools/save_rewrite.py`, `tools/graphics_rewrite.py` | Checked-in transformations anchored to verified guest instructions and addresses. |
| `src/save_menu.c`, `src/graphics_guest.c`, and related headers | Handwritten implementations called by injected hooks. |
| `src/recomp/gen/` | Ignored generated C and dispatch files; inspect but **never hand-edit**. |
| `tests/test_save_rewrite.py`, `tests/test_graphics_rewrite.py`, `tests/test_generate.py` | Unit checks for transformations and generation helpers. |

`pcrecomp/` is a separate, ignored checkout of the upstream lifting tool and
runtime. `PCRECOMP_DIR` can point elsewhere. It is not the Copilot skill in
`.github/skills/pcrecomp/`.

Never delete or modify the user's `Gex/` to make a test pass. Do not distribute
the original game, its assets, or mechanically recovered game code.
Only analyze software the user is authorized to examine. Use available
workspace tools to inspect source and verify CLI flags before recommending or
running commands. Do not claim to have run a command or validated an output
unless it was actually done.

## Generated-code changes

Use this recipe when a change depends on an instruction boundary or hook in
the lifted game. For ordinary handwritten C changes, do not regenerate merely
because generated code exists.

Trace the relevant instruction and its original behavior in the supported
executable/lift. Change the checked-in rewrite and, when needed, the
handwritten hook implementation and declarations. Keep the match specific to
the verified instruction; preserve guest register/flag synchronization and the
non-intercepted control flow. If adding a required hook, update the generator's
relevant presence check.

Regeneration replaces generated `recomp_*.c` files, generated analysis files
under `config/`, and rewrites tracked `config/lift_manifest.json`. Inspect
`git status` and preserve existing changes before starting. Do not overwrite
someone else's generated work or changes to the manifest; resolve a conflict
with the user. Do not modify the original game to satisfy a hash check.

When regeneration is needed and the inputs are available, follow
[generation prerequisites](../../../docs/development.md#generate-the-c-sources).
The script checks the executable digest and pcrecomp revision and fails on
missing required hooks. Do not bypass these checks. If generation fails,
preserve the error; a partially updated output tree is not a successful
regeneration.

Require exit zero; inspect the emitted manifest and resulting hook in
`src/recomp/gen/` against the specific changed anchor and expected count.
A search hit alone is not proof: confirm location, count, control flow and
relevant generator assertions. Review any tracked manifest change.

## Runtime and debugging

For the HLE-based launcher, `src/main.c` parses options, selects and validates
the game directory and executable, initializes the compatibility host and
shims, loads options/saves, selects and initializes rendering/input, and calls
`recomp_host_run()`. Its monitor pumps platform, movie, renderer and input
events and provides smoke-test observation. It is shared by the SDL desktop,
MinGW HLE, Android and Dreamcast paths, with platform-specific setup.
Dreamcast starts at `src/platform/dreamcast/main.c`; Android enters through
`src/platform/android/entry.c` and Java activities. The experimental MSVC
native32 platform has a separate `src/platform/windows/main.c` and runtime
route; do not assume every HLE path applies to it.

For dispatch failures, inspect `RECOMP_ICALL` and the `g_icall_trace` ring.
The portable launcher (Linux and MinGW) prints that ring on fatal signals;
MSVC/native32 uses its VEH callback/exception machinery. Upstream's generic
`runtime/recomp32/main.c` documents the VEH debugging model, but is not this
project's launcher.

Follow the [testing skill](../testing/SKILL.md) and
[build troubleshooting](../../../docs/build-test-troubleshooting.md). A
successful build or rewrite unit test alone does not establish a correct
in-game handoff.

## One-shot workflows

For a brand-new recompilation, mystery / GOG exe triage, 32-bit lift, or Civ /
16-bit DOS:
[PC Static Recompilation](references/new-project.md).
See [tool commands](references/tool-commands.md),
[packaging](references/packaging.md), and [projects](references/projects.md).

## License

pcrecomp is MIT. Users must supply their own binaries; no redistribution of
copyrighted game assets.
