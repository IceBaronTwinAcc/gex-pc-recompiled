---
name: testing-dreamcast
description: 'Dreamcast: build versus Flycast. Compile-only is not Flycast or hardware validation. Emulator checks have separate guest and emulator renderer settings.'
---

# Dreamcast: build versus Flycast

Read the [testing skill](../testing/SKILL.md) and
[project structure](../../../docs/project-structure.md) first.

For build and test routes on Dreamcast, see
[`docs/platform-build-testing.md`](../../../docs/platform-build-testing.md#dreamcast-build-versus-flycast).

## Flycast quick route

For any Dreamcast emulator test, read
[`docs/flycast-testing.md`](../../../docs/flycast-testing.md) before launching.
It explains how to locate and verify the supplied Flycast binary, and provides
an isolated manual-test recipe, required log markers, and failure handling.

Use the user-supplied emulator first, not `/usr/bin/flycast`, an AppImage, or a
new source build. The attachment path is local to this workspace; if missing,
ask for its new location. Do not silently substitute stock Flycast.
Always enable `config:Debug.SerialConsoleEnabled=yes` so guest failures are
visible. Full-MMU initialization alone does not prove the game started.

## Manual tests

For Flycast, select `config:pvr.rend=4` (Vulkan), `config:aica.Volume=100`,
and `audio:backend=auto`. For a Dreamcast PVR test, launch the current build in
Flycast with the guest arguments selecting `--renderer pvr`. Use a separate
manual-test disc/config without autoplay arguments; preserve the normal
production disc and settings. Flycast's Vulkan selection is separate from the
guest's PVR renderer.
