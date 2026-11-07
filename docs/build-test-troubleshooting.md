# Build and test troubleshooting

Use this guide on a build or test failure. Do not change platforms, download
dependencies, clean user data, or reinterpret a skipped test as a pass.
Use [project-structure.md](project-structure.md) for target ownership and the
[testing skill](../.github/skills/testing/SKILL.md) for testing scope, headless
execution, isolation, timeouts, and cleanup.

## Identify the actual build

From the repository root, inspect the intended build directory and its cache:

```sh
test -f build/linux/release/CMakeCache.txt
cmake -LA -N build/linux/release
ctest --test-dir build/linux/release -N
```

Replace the example path with the actual target directory. `cmake -LA -N`
reads the cache; confirm the source root, compiler, build type, `PCRECOMP_DIR`,
`GEX_EXECUTABLE`, platform and renderer selections against the intended run.
Some cache settings may require inspecting `CMakeCache.txt` directly.
`ctest -N` lists configured tests; it does not build or execute them.

## Symptom to next action

| Symptom | Diagnose and recover |
| --- | --- |
| CMake says generated sources are missing | Check `src/recomp/gen/`, supported `Gex/GEX.exe`, the pinned pcrecomp checkout and Python prerequisites. Follow [generated-code changes](generated-code-changes.md) or the [generation prerequisites](development.md#generate-the-c-sources). `gex_codegen` cannot bootstrap an initial configure that already failed. |
| Unsupported executable hash or pcrecomp revision | Compare the actual inputs with `tools/generate.py` and `config/lift_manifest.json`. Ask for the supported authorized installation or toolchain; never edit a hash to force success. |
| Wrong source path, stale cache, relocated build, or unexpected linker errors | Inspect the cache and original configure options. Prefer a new, non-existing build directory with the intended compiler, toolchain and flags; reconfigure and build it. Do not delete, overwrite or silently repurpose an existing build or release artifact. A clean configure is a diagnostic step, not proof the old failure was unrelated. |
| Missing library, compiler or tool | Read the exact configure/build error and the relevant platform manifest. Report the missing prerequisite; install/restore only when appropriate and authorized by the task/environment. Do not change source or use a different backend just to hide the failure. |
| `ctest -N` lacks the expected test | Check `BUILD_TESTING`, platform, renderer flags, cross-compilation, data prerequisites and the `add_test` condition in `CMakeLists.txt` / `cmake/platforms/`. A selector with zero matches is **not** a passing test. Reconfigure the intended profile or report it unavailable. |
| Test executable missing or a Python runner cannot spawn it | Build the configured target first; inspect the command and its path. Verify the executable exists and is runnable. Do not blame the change or claim a pass before finding the cause. |
| Test is skipped, times out or fails | Read full test output, exit code and `SKIP_RETURN_CODE`/timeout configuration. A skip or timeout is not a pass. Diagnose the reported prerequisite or failure and retain a concise log if unresolved. |
| Game files missing or asset validation fails | Check the reported file against `config/game-assets.sha256` and verify the configured installation path. Ask for the supported authorized data; do not alter, synthesize or redistribute retail assets. |

## Build and run a focused selector

After confirming the configuration and prerequisites, for example on a
configured Linux release build:

```sh
cmake --build build/linux/release --parallel 3
ctest --test-dir build/linux/release -N -R '^(password-decks|save-handoff)$'
ctest --test-dir build/linux/release --output-on-failure -R '^(password-decks|save-handoff)$'
```

Choose names for the actual change and require the listing to include every
expected test before running. Platform and feature conditions can remove
tests; inspect the listing, not just CTest's exit code. Use the same safe,
headless settings and platform scope from the testing skill. Escalate only
when the targeted results or change scope require it.

Use a new scratch build for diagnosis only after checking that its resolved
path does not already exist. Record its path; remove only known generated
outputs from that scratch build after use. Preserve `Gex/`, the pcrecomp
checkout, other sessions' files, existing build outputs, and unresolved
failure logs.
