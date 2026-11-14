---
name: testing-android
description: 'Android checks, device-side effects, and evidence. Explicit Android automated test: use a dedicated, explicitly selected headless emulator.'
---

# Android testing: agent runbook

Read the [testing skill](../testing/SKILL.md) and
[project structure](../../../docs/project-structure.md) first.

For Android checks, device-side effects, and evidence, see
[`docs/android-testing.md`](../../../docs/android-testing.md).

Physical device: use only when the user explicitly authorizes that device and
the requested actions. A prior test on a device is not permission to change it
now.
