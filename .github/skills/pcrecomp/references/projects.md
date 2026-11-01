# pcrecomp — project index (from docs/PROJECTS.md)

| Repo | Title | Era | Approach | pcrecomp contributions |
|------|-------|-----|----------|------------------------|
| [sof](https://github.com/sp00nznet/sof) | Soldier of Fortune | 2000 | Clean-room + Win32→SDL2 | `pe_analyze`, `extract_imports`, `isextract`, `win32_compat.h` |
| [xwa](https://github.com/sp00nznet/xwa) | X-Wing Alliance | 1999 | Automated x86→C | `disasm32`, `lift32`, `translator`, `generate`, `safedisc_dump`, `recomp32` runtime, CMake template |
| [heavymetal](https://github.com/sp00nznet/heavymetal) | Heavy Metal FAKK2 | 2000 | Clean-room + SDK refs | `pe_analyze`, `pk3_inspect` |
| [gunman](https://github.com/sp00nznet/gunman) | Gunman Chronicles | 2000 | Ghidra → classify vs HL SDK | `classify/*`, Ghidra scripts |
| [bw](https://github.com/sp00nznet/bw) | Black & White | 2001 | C++ header reconstruction | Ghidra stats, DRM inject, CAB/bin2iso, `cpp/*` |
| [civ](https://github.com/sp00nznet/civ) | Civilization (1991) | 1991 | 16-bit decode→lift→DOS runtime | `decode16`, `analyze`, `lift16`, `recomp16`, `resolve_stubs` |
| [encarta](https://github.com/sp00nznet/encarta) | Encarta 97 | 1996 | Format-first RE | `tools/formats/*` |
| [fallout1-re](https://github.com/sp00nznet/fallout1-re) | Fallout | 1997 | Fork of RE source + extensions | Ecosystem (web/multiplayer), not lift pipeline |
| [fallout2-re](https://github.com/sp00nznet/fallout2-re) | Fallout 2 | 1998 | Upstream RE tracking | Reference |

**Cross-project stats** (from upstream docs): 14k+ functions analyzed, 750k+ lines of generated C, SafeDisc v1/v2 handled, 10+ proprietary formats.

When the user has a `code/` folder in a game repo, treat that as reference/original context; pcrecomp is the **toolbox**, not the game tree.
