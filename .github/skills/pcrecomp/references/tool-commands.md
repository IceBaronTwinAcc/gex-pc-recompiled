# pcrecomp tool commands (verified in repo source)

All paths relative to **pcrecomp repo root**. Read the matching `.py` / `.java` file before advising flags — several README examples differ from current entry points.

## Python dependencies

```bash
pip install capstone pefile lief
```

Python **3.10+**. Ghidra **11.0+** for Java scripts. CMake **3.20+**, VS 2022 (Win32) for runtime builds.

---

## Phase 0 — PE reconnaissance

### `tools/pe/pe_analyze.py`

**Source:** `tools/pe/pe_analyze.py` (CLI at bottom, `analyze_pe()` at line ~155).

```bash
python tools/pe/pe_analyze.py game.exe
python tools/pe/pe_analyze.py game.exe --json config/pe_analysis.json
python tools/pe/pe_analyze.py game.exe --hash
python tools/pe/pe_analyze.py --all ./install_dir/
```

| Flag | Meaning |
|------|---------|
| `--json <path>` | Write machine-readable metadata |
| `--hash` | Section hashes |
| `--all <dir>` | Batch all `.exe`/`.dll` under directory |

### `tools/pe/extract_imports.py`

Standalone script (no argparse). Run:

```bash
python tools/pe/extract_imports.py path/to/game.exe
```

Import table = shim TODO list (`docs/PIPELINE.md` Phase 0).

---

## Phase 1 — Disassembly

### 32-bit: `tools/disasm/disasm32.py`

Library module (`Disassembler`, `Function`). **No `__main__` CLI** in unified repo — used by `tools/lift/translator.py` and game projects. For standalone JSON export, use translator `--functions-json` or game-repo wrappers.

### 16-bit: `tools/disasm/decode16.py`

```bash
python tools/disasm/decode16.py CIV.EXE --resident
python tools/disasm/decode16.py CIV.EXE --overlay 3
python tools/disasm/decode16.py binary.bin 0x200 0x1000
```

### 16-bit: `tools/disasm/analyze.py`

```bash
python tools/disasm/analyze.py CIV.EXE -v
python tools/disasm/analyze.py CIV.EXE -symbols symbols.toml
```

---

## Phase 2 — Classification

Hard-coded paths in Gunman scripts — **edit `SDK_PATH` / `DISASM_PATH`** in script before running.

| Script | Role |
|--------|------|
| `tools/classify/classify_functions.py` | Name-based SDK vs custom |
| `tools/classify/combined_classify.py` | 4-pass (recommended) |
| `tools/classify/deep_classify.py` | String-reference pass |
| `tools/classify/resolve_stubs.py` | CRT/library stub resolution |

Ghidra headless (edit `outputDir` in script):

```bash
analyzeHeadless /project/dir MyProject -import game.dll \
  -postScript tools/ghidra/DecompileAll.java
```

Scripts: `DecompileAll.java`, `ExportFunctions.java`, `GhidraStats.java` under `tools/ghidra/`.

---

## Phase 3 — Lifting

### Full orchestrator: `tools/lift/translator.py`

**Source:** `main()` argparse ~line 179.

```bash
# Intended (when package layout matches XWA project):
python -m tools game.exe --all -o src/recomp/gen --split 1000
python -m tools game.exe --analyze-only --pe-json config/pe.json
python -m tools game.exe --functions-json config/functions.json --stubs src/imports.c
```

| Flag | Meaning |
|------|---------|
| `pe_file` | Positional PE path (not `--exe`) |
| `--all` | Disasm + lift + write `recomp_*.c`, `recomp_funcs.h`, `recomp_dispatch.c` |
| `-o` / `--output` | Default `src/game/recomp/gen` |
| `--split N` | Max functions per `.c` chunk (default 1000) |
| `--analyze-only` | PE analysis only |
| `--pe-json` | Export PE metadata |
| `--functions-json` | Export function list |
| `--stubs` | Generate import bridge stub file |

**Entry point:** From repo root, `python -m tools --help` (see `references/packaging.md`).

### Fast linear sweep: `tools/lift/generate.py`

```bash
python tools/lift/generate.py game.exe src/recomp/gen 500
```

Positional: `exe`, `output_dir`, optional `split_size`. Imports `tools.pe_analyze` and `tools.lifter` — requires PYTHONPATH/package fix (see packaging doc).

### 16-bit: `tools/lift/lift16.py`

Used after `decode16` + `analyze`; see `docs/PIPELINE.md` and Civ project layout.

---

## DRM

### `tools/drm/safedisc_dump.py`

```bash
python tools/drm/safedisc_dump.py game.exe decrypted.exe
python tools/drm/safedisc_dump.py --pid 12345 game.exe decrypted.exe
```

(No `--exe` / `--output` flags in source — README examples may differ.)

### `tools/drm/inject_and_run.c`

Build and inject for SafeDisc v2+ analysis (Black & White project).

---

## Assets & formats

| Tool | Use |
|------|-----|
| `tools/assets/isextract.py` | InstallShield `.hdr` + `.cab` |
| `tools/assets/pk3_inspect.py` | Quake/id PK3/ZIP |
| `tools/assets/bin2iso.js` | BIN/CUE → ISO |
| `tools/assets/extract_cab.sh` | CAB extraction |
| `tools/formats/*/` | FIF, M20, SPAM, DAT, STR decoders (C tools, compile per dir) |
| `tools/cpp/*.py`, `parse_vtables.js` | MSVC/MWerks mangling, vtables |

---

## Runtime & templates

Copy into game project:

- **32-bit:** `runtime/recomp32/recomp_types.h`, `runtime/recomp32/main.c`
- **16-bit:** `runtime/recomp16/` (CPU, `dos_compat`, HAL, SDL2)
- **Win32 shims:** `runtime/compat/win32_compat.h`
- **CMake:** `templates/CMakeLists.txt.template` (`/DYNAMICBASE:NO`, `/STACK:8388608`, `file(GLOB ... recomp_*.c)`)

Template `codegen` target expects `python -m tools --all` — fix module path or call `translator.py` after packaging fix.
