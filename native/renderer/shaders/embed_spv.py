#!/usr/bin/env python3
"""embed_spv.py — convert a .spv file to a checked-in .inc u32 array.

Usage: python3 embed_spv.py <in.spv> <out.inc>
Writes the same header format as the checked-in cull_spv.inc/hiz_spv.inc.
"""
import pathlib
import sys

template = """// {stem}.comp.spv — {n} words, build-embedded (see shaders/README).
// Regenerate: glslangValidator -V --target-env vulkan1.3 -o {stem}.spv {stem}.comp
// then: python3 native/renderer/shaders/embed_spv.py {stem}.spv {stem}_spv.inc
#pragma once
#include <cstdint>
inline constexpr std::uint32_t k{cap}SpvWords = {n}u;
inline constexpr std::uint32_t k{cap}Spv[] = {{
{body}
}};
"""


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: embed_spv.py <in.spv> <out.inc>")
    spv = pathlib.Path(sys.argv[1]).read_bytes()
    assert spv[:4] == b"\x03\x02#\x07", "not SPIR-V"
    assert len(spv) % 4 == 0, "SPIR-V length not a multiple of 4"
    n = len(spv) // 4
    words = [int.from_bytes(spv[i * 4:(i + 1) * 4], "little")
             for i in range(n)]
    body = "\n".join(
        "    " + " ".join(f"0x{w:08x}u," for w in words[i:i + 6])
        for i in range(0, n, 6))
    stem = pathlib.Path(sys.argv[1]).stem
    cap = stem.capitalize()
    pathlib.Path(sys.argv[2]).write_text(
        template.format(stem=stem, n=n, cap=cap, body=body))


if __name__ == "__main__":
    main()
