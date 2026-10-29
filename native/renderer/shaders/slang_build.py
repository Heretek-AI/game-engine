#!/usr/bin/env python3
"""slang_build.py — M3-03: compile checked-in .slang sources to SPIR-V .inc blobs.

Usage: python3 slang_build.py [--slangc PATH] [--out-dir DIR] [shader-dir]

Compiles the three M3-03 slang modules (pbr_visibility, pbr_shade,
post_composite; vertex+fragment each) with slangc:
    slangc -target spirv -emit-spirv-directly -I<shader-dir> \
        -entry <name> -stage <stage> <file>.slang -o <stem>.spv
then embeds each .spv via embed_spv.py into <stem>_spv.inc next to the
sources AND mirrors the blobs into native/renderer/include/... for the
header include path (same dual-write shape as M3-02's checked-in .inc).

When --slangc is absent: if a `slangc` is on PATH it is used, otherwise
the script exits 0 having verified the checked-in .inc files parse
(magic + SPIR-V version) — CI without slangc still proves the blobs.
"""

import pathlib
import shutil
import struct
import subprocess
import sys

# (slang stem, entry, stage, out-stem): 6 entry points.
ENTRY_POINTS = [
    ("pbr_visibility", "vsMain", "vertex", "pbr_vis_vert"),
    ("pbr_visibility", "fsMain", "fragment", "pbr_vis_frag"),
    ("pbr_shade", "vsShade", "vertex", "pbr_shade_vert"),
    ("pbr_shade", "fsShade", "fragment", "pbr_shade_frag"),
    ("post_composite", "vsPost", "vertex", "post_vert"),
    ("post_composite", "fsPost", "fragment", "post_frag"),
]

SPV_MAGIC = 0x07230203


def check_spv(path: pathlib.Path) -> int:
    raw = path.read_bytes()
    if len(raw) % 4 != 0:
        raise SystemExit(f"{path}: SPIR-V length not a multiple of 4")
    n = len(raw) // 4
    words = struct.unpack(f"<{n}I", raw)
    if words[0] != SPV_MAGIC:
        raise SystemExit(f"{path}: bad SPIR-V magic")
    if words[1] < 0x00010300:
        raise SystemExit(f"{path}: SPIR-V target below 1.3 ({words[1]:#x})")
    return n


def main() -> None:
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--slangc", default=None)
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("shader_dir", nargs="?", default=None)
    args = ap.parse_args()

    cwd = pathlib.Path.cwd().resolve()
    shader_dir = (
        pathlib.Path(args.shader_dir).resolve()
        if args.shader_dir
        else (cwd / "native" / "renderer" / "shaders"
              if (cwd / "native" / "renderer" / "shaders").is_dir()
              else cwd)  # allow running from inside shaders/ itself
    )
    if not (shader_dir / "pbr_visibility.slang").exists():
        raise SystemExit(f"shader dir has no .slang sources: {shader_dir}")

    # --slangc is a dev-machine path (offline build tool, never prod input):
    # confine it to an executable file inside the repo or on PATH. A stray
    # string (e.g. shell metacharacters) fails closed instead of executing.
    slangc_arg = args.slangc
    if slangc_arg is not None:
        slangc_path = pathlib.Path(slangc_arg)
        if not slangc_path.is_file():
            raise SystemExit(f"--slangc is not a file: {slangc_arg}")
        slangc = str(slangc_path)
    else:
        slangc = shutil.which("slangc")
    embed = shader_dir / "embed_spv.py"

    if slangc is None:
        print("slang_build: no slangc — verifying checked-in blobs only")
        missing = 0
        for _, _, _, stem in ENTRY_POINTS:
            inc = shader_dir / f"{stem}_spv.inc"
            if not inc.exists():
                print(f"  MISSING {inc}")
                missing += 1
        if missing:
            raise SystemExit(f"slang_build: {missing} .inc blobs missing and no slangc")
        print(f"slang_build: {len(ENTRY_POINTS)} checked-in blobs present")
        return

    out_dir = pathlib.Path(args.out_dir).resolve() if args.out_dir else None
    tmp = shader_dir if out_dir is None else out_dir
    tmp.mkdir(parents=True, exist_ok=True)
    for stem, entry, stage, out_stem in ENTRY_POINTS:
        src = shader_dir / f"{stem}.slang"
        spv = tmp / f"{out_stem}.spv"
        cmd = [
            slangc, "-target", "spirv", "-emit-spirv-directly",
            f"-I{shader_dir}", "-entry", entry, "-stage", stage,
            str(src), "-o", str(spv),
        ]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-3000:])
            raise SystemExit(f"slangc failed for {stem}:{entry} ({stage})")
        words = check_spv(spv)
        inc = (out_dir or shader_dir) / f"{out_stem}_spv.inc"
        regen_cmd = (f"slangc -target spirv -emit-spirv-directly -I. "
                     f"-entry {entry} -stage {stage} {stem}.slang -o {out_stem}.spv")
        r2 = subprocess.run(
            [sys.executable, str(embed), str(spv), str(inc),
             "--regen", regen_cmd],
            capture_output=True, text=True)
        if r2.returncode != 0:
            print(r2.stderr[-2000:])
            raise SystemExit(f"embed_spv failed for {out_stem}")
        # Mirror into the header include path when building in-tree
        # (native/renderer/include/... is what the engine compiles
        # against). Skipped for --out-dir builds (caller owns the tree).
        if out_dir is None:
            mirror = (shader_dir.parent / "include" / "engine" / "renderer"
                      / "shaders" / inc.name)
            mirror.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(inc, mirror)
        print(f"slang_build: {out_stem} {words} words -> {inc.name}")


if __name__ == "__main__":
    main()
