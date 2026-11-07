# Generated-code changes

Use this recipe when a change depends on an instruction boundary or hook in
the lifted game. For ordinary handwritten C changes, do not regenerate merely
because generated code exists. See [project-structure.md](project-structure.md)
for the overall pipeline and [development.md](development.md#generate-the-c-sources)
for prerequisites.

## What is owned where

| Component | Role |
| --- | --- |
| `Gex/GEX.exe` and the separate `pcrecomp/` checkout | Inputs validated by `tools/generate.py`; the original executable is user-owned. |
| `tools/generate.py` | Runs the lifter, calls rewrites, validates hooks and writes generated output. |
| `tools/save_rewrite.py`, `tools/graphics_rewrite.py` | Checked-in transformations anchored to verified guest instructions and addresses. |
| `src/save_menu.c`, `src/graphics_guest.c`, and related headers | Handwritten implementations called by injected hooks. |
| `src/recomp/gen/` | Ignored generated C and dispatch files; inspect but **never hand-edit**. |
| `tests/test_save_rewrite.py`, `tests/test_graphics_rewrite.py`, `tests/test_generate.py` | Unit checks for transformations and generation helpers. |

Regeneration replaces generated `recomp_*.c` files, generated analysis files
under `config/`, and rewrites tracked `config/lift_manifest.json`. Inspect
`git status` and preserve existing changes before starting. Do not overwrite
someone else's generated work or changes to the manifest; resolve a conflict
with the user. Do not modify the original game to satisfy a hash check.

## Example: a save/title hook

1. Trace the relevant instruction and its original behavior in the supported
   executable/lift. In `tools/save_rewrite.py`, `TITLE` is the anchor for the
   title dispatcher and `TITLE_HOOK` injects `gex_save_choose()`. That function
   is implemented in `src/save_menu.c`. `tools/generate.py` calls
   `rewrite_saves()` and checks for several required save hooks.
2. Change the checked-in rewrite and, when needed, the handwritten hook
   implementation and declarations. Keep the match specific to the verified
   instruction; preserve guest register/flag synchronization and the
   non-intercepted control flow. If adding a required hook, update the
   generator's relevant presence check.
3. Extend `tests/test_save_rewrite.py` with a minimal input fixture that checks
   the intended insertion, a nearby nonmatching instruction, and idempotence
   (`rewrite_saves(rewrite_saves(input))` equals the first result). Test
   behavior in the relevant C/integration tests too; a text match alone does
   not prove runtime correctness.
4. Run focused Python unit tests from the repository root:

   ```sh
   .venv/bin/python -m unittest discover -s tests -p 'test_save_rewrite.py'
   ```

   For a graphics rewrite, substitute `test_graphics_rewrite.py`; for
   generator helpers, use `test_generate.py`.
5. When regeneration is needed and the inputs are available, follow
   [generation prerequisites](development.md#generate-the-c-sources) and run:

   ```sh
   .venv/bin/python tools/generate.py
   ```

   Use `--exe` and `--toolchain` for nondefault locations. The script checks
   the executable digest and pcrecomp revision and fails on missing required
   hooks. Do not bypass these checks. If generation fails, preserve the error;
   a partially updated output tree is not a successful regeneration.
6. Require exit zero; inspect the emitted manifest and resulting hook in
   `src/recomp/gen/` against the specific changed anchor and expected count.
   The existing title hook can be located with:

   ```sh
   rg -n -C 6 'gex_save_choose\(' src/recomp/gen -g 'recomp_*.c'
   git diff -- config/lift_manifest.json
   ```

   A search hit alone is not proof: confirm location, count, control flow and
   relevant generator assertions. Review any tracked manifest change.
7. Reconfigure if needed, build the selected current-host target and run the
   focused relevant CTest tests, such as `password-decks` and `save-handoff`
   for this example. Follow the [testing skill](../.github/skills/testing/SKILL.md)
   and [build troubleshooting](build-test-troubleshooting.md). A successful
   build or rewrite unit test alone does not establish a correct in-game handoff.

For graphics, the analogous path is `tools/graphics_rewrite.py` →
`tools/generate.py` → generated hooks → `src/graphics_guest.c` and the
selected renderer. Validate the specific transformation and backend rather
than assuming a save-hook test covers it.
