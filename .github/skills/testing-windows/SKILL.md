---
name: testing-windows
description: 'Windows: MinGW x64 and isolated Wine. Wine results do not prove native Windows display, audio, controller behavior or a full playthrough.'
---

# Windows: MinGW x64 and isolated Wine

Read the [testing skill](../testing/SKILL.md) and
[project structure](../../../docs/project-structure.md) first.

For build and test routes on Windows, see
[`docs/platform-build-testing.md`](../../../docs/platform-build-testing.md#windows-mingw-x64-and-isolated-wine).

Automated Wine tests must also be silent. `SDL_AUDIODRIVER=dummy` does not mute
native DirectSound. Before starting Wine in a disposable prefix, disable
`winepulse.drv,winealsa.drv,wineoss.drv` in `WINEDLLOVERRIDES` and set that
prefix's `HKCU\Software\Wine\Drivers\Audio` value to an empty string. Keep
SDL's dummy audio driver for the software fallback. Never mute the user's
normal Wine prefix or change the host's sound settings.
