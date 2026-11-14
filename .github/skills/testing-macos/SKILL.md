---
name: testing-macos
description: 'macOS: separate architectures and runtime. Cross-compilation or Mach-O inspection alone is not a Mac runtime test.'
---

# macOS: separate architectures and runtime

Read the [testing skill](../testing/SKILL.md) and
[project structure](../../../docs/project-structure.md) first.

For build and test routes on macOS, see
[`docs/platform-build-testing.md`](../../../docs/platform-build-testing.md#macos-separate-architectures-and-runtime).

The normal macOS bgfx backend selects Metal; do not claim a Vulkan manual test
occurred if the required configuration is unavailable.
