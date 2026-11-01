# pcrecomp Python packaging

Run all module commands from the **pcrecomp repo root** (directory containing `tools/`).

## Primary entry point

```bash
python -m tools game.exe --all -o src/recomp/gen --split 1000
python -m tools --exe game.exe --analyze-only --pe-json config/pe_analysis.json
python tools/lift/generate.py game.exe src/recomp/gen 500
```

`tools/__main__.py` delegates to `tools.lift.translator`. Imports use the `tools.pe`, `tools.disasm`, `tools.lift` packages.

## Standalone scripts (no `-m tools`)

- `python tools/pe/pe_analyze.py <pe> [--json ...]`
- `python tools/pe/extract_imports.py <pe>`
- `python tools/disasm/decode16.py ...`
- `python tools/disasm/analyze.py ...`
- `python tools/drm/safedisc_dump.py <exe> [out.exe]`

## If imports fail

Ensure cwd is repo root. On Windows:

```powershell
cd E:\path\to\pcrecomp
$env:PYTHONPATH = (Get-Location).Path
python -m tools --help
```

Do not run `python tools/lift/translator.py` directly (use `-m tools`).
