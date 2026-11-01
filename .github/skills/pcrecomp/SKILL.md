---
name: pcrecomp
description: "Guide PC static recompilation of vintage Windows/DOS x86 binaries with sp00nznet/pcrecomp: PE recon, 32-bit and 16-bit lifting, classification, shims, assets, and builds. Use for old .exe/.dll triage, native recompilation, crash/debug loops, or Quake/GoldSrc/id Tech/MFC/DOS ports. Do not use for Xbox/N64/mobile APK, DOSBox/Wine-only emulation, web apps, or non-x86 PC targets."
license: MIT
---

# PC Static Recompilation (pcrecomp)

Help users work with **[pcrecomp](https://github.com/sp00nznet/pcrecomp)** — the unified toolbox for PC static recompilation (PE/DOS analysis → disasm → classify → lift → shim → build → debug).

Adapted for GitHub Copilot from the [DohmBoy64Bit/pcrecomp-skill](https://github.com/DohmBoy64Bit/pcrecomp-skill) skill. This project-local skill is installed at `.github/skills/pcrecomp/`; supporting command, packaging, and project references are in the adjacent `references/` directory.

### Copilot workflow

- Apply this skill when the user asks for PC x86 static recompilation, binary triage, or closely related tooling. If the task is outside that scope, do not force this workflow.
- Treat the current workspace and any user-specified paths as authoritative. This skill folder is not the pcrecomp toolkit; locate the user's actual toolkit clone before relying on its `tools/`, `runtime/`, or `docs/`.
- If no toolkit clone is available, explain what source or path is needed rather than assuming these files or the game-data folder contain the toolkit.
- Use available workspace tools to inspect source and verify CLI flags before recommending or running commands. Do not claim to have run a command or validated an output unless it was actually done.
- Only analyze software the user is authorized to examine. Do not copy retail binaries into this skill or commit them. State environment limits when Windows-only build steps cannot be verified in the current environment.
- Read `references/tool-commands.md`, `references/packaging.md`, and `references/projects.md` when they apply; these are supporting references, not a substitute for checking the user's toolkit source.

**Upstream source of truth:** If a local clone exists (e.g. `pcrecomp/` or `e:\SkillDev\PC\pcrecomp`), read `README.md`, `docs/PHILOSOPHY.md`, `docs/PIPELINE.md`, and `docs/PROJECTS.md` **before** advising. Verify every CLI flag in the matching file under `tools/` — do not invent APIs.

**Game-specific trees** (Soldier of Fortune, XWA, Civ, Gunman, etc.) live in separate repos; per-game `code/` folders are reference context. pcrecomp holds **reusable** tools and runtimes.

### Verified pitfalls (check the clone, not README alone)

| Topic | Correct (source) | Wrong assumption |
|-------|------------------|------------------|
| **16-bit Civ path** | `analyze.py CIV.EXE` takes the **exe** (`analyze.py` ~362) | README chain `decode16 → analyze decoded.json` |
| **Imports for unknown exe** | `pe_analyze.py --json` → `iat_map` / import list | `extract_imports.py` `__main__` is SoF-specific |
| **32-bit lift entry** | From repo root: `python -m tools game.exe --all -o src/recomp/gen` | `python tools/lift/translator.py` as a script |
| **SafeDisc dump** | Positional: `safedisc_dump.py game.exe out.exe` | `--exe` / `--output` flags |

Before advising a command, run `python -m tools --help` or read the tool file in the user's clone.

For a requested report or document deliverable, use `response.md` unless the user specifies another path. Do not create a report file for ordinary coding or Q&A tasks.

### Agent response checklist (include in answers)

| User goal | Must mention |
|-----------|----------------|
| Mystery / GOG exe triage | `pe_analyze.py --json`, **do not** rely on `extract_imports.py` CLI (SoF-hardcoded), Phase 0 before lift |
| 32-bit lift complete | `python -m tools … --all -o src/recomp/gen`, **`recomp_dispatch.c`**, `/DYNAMICBASE:NO`, **`-A Win32`**, **`g_icall_trace`** + **VEH** in `runtime/recomp32/main.c` |
| Civ / 16-bit DOS | `analyze.py CIV.EXE` (not README `decoded.json` chain), `runtime/recomp16`, **no** `python -m tools` |

---

## Mental model

| Layer | Role |
|-------|------|
| **tools/** | Analysis & transformation (PE, disasm, lift, classify, Ghidra, DRM, assets, formats, C++) |
| **Generated `src/.../recomp/gen/`** | Mechanical C (`recomp_*.c`, `recomp_dispatch.c`, `recomp_funcs.h`) |
| **runtime/** | `recomp32` (global registers, dispatch), `recomp16` (DOS INTs + SDL2), `compat/win32_compat.h` |
| **Game project** | `main.c`, memory map, manual overrides, CMake linking generated sources |

Static recomp ≠ emulator: lifted functions are native C. Most calendar time is **build & debug** (Phase 5), not lifting.

**Universal pipeline** (from `docs/PHILOSOPHY.md`):

```
Analyze → Disassemble → Classify → Lift → Shim → Build → Debug → Ship
```

---

## Choose a path (decision tree)

```
Source code available (SDK, leak, port)?
  YES → Source port / SDK diff; use classify tools to skip SDK functions
  NO  → Continue

16-bit MZ / DOS?
  YES → decode16 → analyze → lift16 → runtime/recomp16
  NO  → Continue

32-bit PE?
  YES → DRM (SafeDisc sections, packed .text)?
        YES → safedisc_dump.py (or inject_and_run.c for v2+) then lift
        NO  → pe_analyze → disasm32 path → lift32 / translator / generate
  NO  → Other platform (N64/Xbox/etc. — sibling repos, not pcrecomp)

Quake / GoldSrc / public engine?
  YES → Ghidra DecompileAll → combined_classify vs SDK → reconstruct custom only

C++ heavy (vtables, RTTI)?
  YES → GhidraStats + msvc_mangler + parse_vtables.js

Multimedia / proprietary formats only?
  YES → tools/formats/* before app code (Encarta pattern)
```

Full narrative: `docs/PHILOSOPHY.md` § "When to Use Which Approach".

---

## Phase 0 — Reconnaissance (always first)

**Why:** The binary reports compiler era, image base, relocations, DRM, and the full import table (shim checklist). ~10 minutes; saves weeks of wrong assumptions (`docs/PIPELINE.md`).

```bash
python tools/pe/pe_analyze.py mystery.exe
python tools/pe/pe_analyze.py mystery.exe --json config/pe_analysis.json
python tools/pe/extract_imports.py mystery.exe
```

**Read in output:** `TimeDateStamp`, linker version, `ImageBase`, `.reloc`, section names (UPX, `.sforce`), import DLLs.

**Docs:** `docs/PIPELINE.md` Phase 0 table.

---

## Phase 1 — Disassembly

### 32-bit Win32

**Tool:** `tools/disasm/disasm32.py` — Capstone recursive descent, prologue scan (`55 8B EC`), call-target discovery, basic blocks.

Consumed by `tools/lift/translator.py` (`Disassembler.find_functions`). No standalone CLI in monorepo; export via translator `--functions-json`.

### 16-bit DOS (e.g. Civ 1991)

```bash
python tools/disasm/decode16.py GAME.EXE --resident
python tools/disasm/analyze.py GAME.EXE -v -symbols symbols.toml
```

Then `tools/lift/lift16.py` → `RecompiledFuncs/` (per README).

**Docs:** `docs/PIPELINE.md` Phase 1.

---

## Phase 2 — Classification

**Why:** Separates SDK/engine code from custom logic (Gunman: 78% SDK → ~499 functions to RE).

**Tools:** `combined_classify.py` (4 passes: names, strings, call-graph propagation, address clustering), plus `classify_functions.py`, `deep_classify.py`, `resolve_stubs.py`.

**Ghidra batch decompile** (`tools/ghidra/DecompileAll.java`):

```bash
analyzeHeadless /path/to/ghidra_proj MyProj -import game.dll \
  -postScript tools/ghidra/DecompileAll.java
```

Edit hard-coded `outputDir` in the Java file for your machine.

**Docs:** `docs/PIPELINE.md` Phase 2; example ratios in `docs/PROJECTS.md` (Gunman, XWA, Civ).

---

## Phase 3 — Lifting (x86 → C)

### Design (32-bit)

From `docs/PIPELINE.md` and `runtime/recomp32/recomp_types.h`:

- Global registers: `g_eax` … `g_esp` (preserves original ABI without recovering calling conventions)
- Memory: `MEM32(va)` via `g_mem_base`
- Conditions: flag-setter → consumer pattern (`CMP_EQ`, etc. in lifter)
- FPU: `_st[8]` stack in each function
- Indirect calls: `RECOMP_ICALL(target)` → `recomp_lookup_manual` → `recomp_lookup` → `recomp_lookup_import`

**Example ICALL macro** (source: `runtime/recomp32/recomp_types.h` ~line 254):

```c
#define RECOMP_ICALL(target_va) do { \
    uint32_t _va = (uint32_t)(target_va); \
    /* trace ring buffer */ \
    recomp_func_t _fn = recomp_lookup_manual(_va); \
    ...
} while(0)
```

### Orchestrator (`tools/lift/translator.py`)

Documented CLI (positional PE, not `--exe`):

```bash
python -m tools game.exe --all -o src/recomp/gen --split 1000
python -m tools game.exe --pe-json config/pe_analysis.json --analyze-only
```

**Outputs when `--all`:** `recomp_NNNN.c`, `recomp_funcs.h`, `recomp_dispatch.c` (sorted VA table for binary search).

**Fast path:** `tools/lift/generate.py` — linear sweep (trades accuracy for speed).

**Packaging:** Run from pcrecomp repo root: `python -m tools --help`. See `references/packaging.md`.

**Docs:** `docs/PIPELINE.md` Phase 3.

---

## Phase 4 — Shimming

| Target | Resource |
|--------|----------|
| Win32 → modern/SDL2 | `runtime/compat/win32_compat.h` (KEEP / SHIM / SDL2 / STUB / CRT) |
| DOS INTs, VGA, ports | `runtime/recomp16/dos_compat.c`, HAL under `recomp16/hal/` |
| SafeDisc v1 | `tools/drm/safedisc_dump.py` — run decrypted game, dump `.text` |
| SafeDisc v2+ | `tools/drm/inject_and_run.c` |

Philosophy: let DRM decrypt at runtime, then dump (`docs/PHILOSOPHY.md` § DRM).

---

## Phase 5 — Build & debug

### CMake template

Copy `templates/CMakeLists.txt.template` + `.gitignore.template`:

- `/DYNAMICBASE:NO` — fixed VAs for data sections
- `/STACK:8388608` — deep lifted call chains
- `file(GLOB "src/recomp/gen/recomp_*.c")`
- Build **Win32**: `cmake -B build/windows/msvc -G "Visual Studio 17 2022" -A Win32`

### Runtime (`runtime/recomp32/`)

- `main.c` — VirtualAlloc data map, stack setup, VEH handler, entry via `recomp_lookup(entry_rva)()`
- `recomp_types.h` — registers, `MEM32`, dispatch, `RECOMP_ICALL`, ICALL trace ring (32 entries)

### Debug loop

```
Build → Run → Crash → ICALL trace + VEH dump → Fix → Repeat
```

When documenting a completed 32-bit lift, always mention post-build debugging: **`RECOMP_ICALL`** / `g_icall_trace` ring buffer and the **VEH** handler in `runtime/recomp32/main.c` (see `recomp_types.h`).

| Symptom | Likely fix |
|---------|------------|
| Dispatch miss | Add manual override or missing lifted function |
| AV on data VA | Section load / `g_mem_base` / image base mismatch |
| Unimplemented import | Import bridge in stub file (`translator --stubs`) |
| Wrong branch | Lifter condition pattern; compare to disasm |

**Docs:** `docs/PIPELINE.md` Phase 5.

---

## Phase 6 — Ship

Native exe on modern Windows (or SDL2-backed POSIX for DOS/Win32-compat paths). Extensions (widescreen, net play) are normal once shims exist — see Fallout 1 RE ecosystem in `docs/PROJECTS.md`.

---

## One-shot workflows (README quick start)

### Mystery exe

```bash
python tools/pe/pe_analyze.py mystery.exe --json analysis.json
python tools/pe/extract_imports.py mystery.exe
```

### 32-bit exe → C (intended)

```bash
python -m tools game.exe --all --output src/recomp/gen/
# Stepwise:
python tools/pe/pe_analyze.py game.exe --json config/pe_analysis.json
# disasm32 via translator; lift via --all
```

Use **actual** `-o` not README's `--output` if argparse rejects it (verify `translator.py`).

### 16-bit DOS

```bash
python tools/disasm/decode16.py GAME.EXE --output decoded.json   # README; verify decode16 CLI
python tools/disasm/analyze.py decoded.json                    # README path — analyze.py expects .exe in source
python tools/lift/lift16.py functions.json --output RecompiledFuncs/
```

**Note:** `analyze.py` CLI takes `civ.exe` directly in source; README's `decoded.json` chain may differ — read `decode16.py` / `lift16.py` in clone.

### SafeDisc

```bash
python tools/drm/safedisc_dump.py game.exe decrypted.exe
```

README sometimes shows `--exe` / `--output`; **source uses positional args** (`tools/drm/safedisc_dump.py` `__main__` ~line 306).

---

## Supporting tools (by task)

| Task | Tool |
|------|------|
| InstallShield | `tools/assets/isextract.py` |
| PK3 / ZIP | `tools/assets/pk3_inspect.py` |
| BIN/CUE → ISO | `tools/assets/bin2iso.js` |
| FIF / M20 / SPAM / DAT | `tools/formats/*` (compile C decoders) |
| MSVC mangling | `tools/cpp/msvc_mangler.py` |
| Vtables | `tools/cpp/parse_vtables.js` |

Full table: `references/tool-commands.md`.

---

## Principles (apply on every project)

1. **Binary first** — never skip `pe_analyze.py`.
2. **Classify before lift** — shrink RE surface (`docs/PHILOSOPHY.md` §2).
3. **Correctness over readability** — ugly lifted C is fine.
4. **Global register model** — don't fight the original ABI.
5. **Three-tier dispatch** — manual overrides, auto table, import bridges.
6. **Shim layer is the product** — especially Win32/DOS API mapping.
7. **Tools compound** — each project refines shared scripts.

---

## Related repos

| Need | Repo |
|------|------|
| Automated 32-bit pipeline reference | [xwa](https://github.com/sp00nznet/xwa) |
| 16-bit DOS runtime proof | [civ](https://github.com/sp00nznet/civ) |
| GoldSrc classify + Ghidra | [gunman](https://github.com/sp00nznet/gunman) |
| Win32 compat catalog | [sof](https://github.com/sp00nznet/sof) |

Index: `references/projects.md`.

---

## Agent checklist

When helping with a pcrecomp task:

1. Confirm local clone path; read relevant `docs/*.md`.
2. Run **Phase 0** `pe_analyze` (and DRM dump if needed).
3. Pick pipeline branch (16 vs 32, SDK vs full lift).
4. Verify CLI in source; cite file + line when telling the user a command.
5. Copy runtime + CMake template; set image base / data VAs from PE JSON.
6. Plan iterative ICALL / import debugging — set expectations on timeline.

**Packaging issues:** `references/packaging.md`.

**Command reference:** `references/tool-commands.md`.

---

## License

pcrecomp is MIT. Users must supply their own binaries; no redistribution of copyrighted game assets.
