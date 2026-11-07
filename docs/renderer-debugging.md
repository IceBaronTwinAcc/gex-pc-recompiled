# Renderer debugging and evidence

Use this guide for a visual regression. First read
[project-structure.md](project-structure.md), then the
[testing skill](../.github/skills/testing/SKILL.md). Start with a specific
scene, expected result and target backend; avoid treating a frame counter as
a picture. For Dreamcast emulator work, use [flycast-testing.md](flycast-testing.md)
and only run it when the user explicitly requested that target.

## Trace the layers

| Layer | Starting points | What evidence can establish |
| --- | --- | --- |
| Lifted guest and graphics hooks | `tools/graphics_rewrite.py`, `src/graphics_guest.c` | Hook placement, guest draw/transition behavior and guest-memory inputs. A rewrite match alone does not prove rendered pixels. |
| Raster/packet operations | `src/raster.c`, `src/packet_gpu.h`, selected `src/platform/*/packet_gpu.c` | Packet interpretation, texture/palette and backend output. Compare the same inputs and selected backend; a unit fixture may not cover a game scene. |
| Framebuffer interface | `src/renderer.c`, `src/renderer.h`, `src/renderer_headless.c` | CPU-readable pixel content when a framebuffer is actually available. A dummy/headless framebuffer is not the desktop window or emulator scanout. |
| Host presentation | `src/platform/desktop/renderer.c`, `src/platform/desktop/present.c`, or selected platform presenter | Presentation and event-thread behavior. Successful presentation or a nonzero frame count does not prove pixels are correct. |
| GPU-owned output / emulator scanout | `src/platform/dreamcast/packet_gpu.c`, `src/platform/bgfx/`, Flycast | Final visual output must be inspected at the presentation/scanout surface. GPU-owned frames may have no CPU pixels, and CPU screenshot or pixel-based gameplay verification is rejected in that mode. |

`--renderer pvr` selects the guest packet backend; Flycast
`config:pvr.rend=4` selects the emulator's Vulkan host renderer. They are
independent. Select supported backends for the configured platform; do not
silently switch backends and report that the original one works.

## Narrow a regression

1. Record platform, actual build/configuration, backend, game data, scene,
   viewport and settings. Reproduce both sides with the same conditions,
   including movie-to-menu handoff for a transition bug.
2. Check the relevant CTest registrations and run focused tests after building:
   `renderer-contract`, `renderer-memory-backend`, `guest-graphics`,
   `guest-packet-gpu`, `packet-gpu-differential`, or `presentation-thread`,
   as appropriate and available. See
   [build troubleshooting](build-test-troubleshooting.md). A skipped GPU
   test is not a pass; GPU tests may need a headless GL-capable driver rather
   than SDL's dummy video driver.
3. For CPU framebuffer investigations, use the testing skill's isolated,
   bounded headless run and `--screenshot` only for a specific comparison.
   Inspect the image pixels and actual scene on both sides, not just file
   existence, checksum or frame count. A black or unrelated frame can still
   have a different checksum.
4. For GPU-owned rendering, inspect an appropriate external output/scanout
   capture under equivalent conditions. Do not use the unsupported guest CPU
   screenshot or pixel-based `--verify-gameplay` as a proxy. Automated tests
   remain headless and silent; a visible fullscreen test requires an explicit
   manual request and follows the testing skill.
5. Report separately what was observed: hook/unit behavior, guest pixels,
   host presentation or emulator scanout. A screenshot does not establish
   collision or complete gameplay, and one scene does not cover transitions
   or all backends. Preserve evidence only while it is needed.

The long-form implementation details and historical measurements are in
[development.md](development.md#modular-renderer); verify the current source
and configuration before treating historical results as current behavior.
