#!/usr/bin/env python3
"""Summarize a display-list dump from the 3DS (switch "dldump", written by n3ds_calls.c).

Usage: python 3ds/tools/dlparse.py build3ds/dl_frame.bin

File: three little-endian u32 per command (opcode, w0, w1). Prints the opcode histogram and
the counts that matter for the frame-time question: triangles, matrix loads, DL jumps, state.
"""
import struct
import sys
from collections import Counter

NAMES = {
    0x00: "NOOP", 0x01: "VTX", 0x05: "TRI1", 0x06: "TRI2", 0x07: "QUAD", 0x09: "TRIN",
    0x0A: "TRIN_INDEPEND", 0x0D: "LINE3D", 0xD7: "TEXTURE", 0xD8: "POPMTX", 0xD9: "GEOMETRYMODE",
    0xDA: "MTX", 0xDC: "MOVEMEM", 0xDE: "DL", 0xDF: "ENDDL", 0xE1: "RDPHALF1", 0xE2: "SETOTHERMODE_L",
    0xE3: "SETOTHERMODE_H", 0xE4: "TEXRECT", 0xE7: "PIPESYNC", 0xF0: "LOADTLUT", 0xF2: "SETTILESIZE",
    0xF3: "LOADBLOCK", 0xF5: "SETTILE", 0xFC: "SETCOMBINE", 0xFD: "SETTIMG",
}
TRI_OPS = {0x05: 1, 0x06: 2, 0x07: 2, 0x09: 0, 0x0A: 0}  # TRIN ops carry their own face count


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 1
    data = open(sys.argv[1], "rb").read()
    n = len(data) // 12
    cmds = struct.unpack("<%dI" % (n * 3), data[: n * 12])
    ops = cmds[0::3]
    hist = Counter(op & 0xFF for op in ops)

    print("commands: %d" % n)
    print("%-6s %-16s %8s" % ("op", "name", "count"))
    for op, count in hist.most_common():
        print("0x%02X   %-16s %8d" % (op, NAMES.get(op, "?"), count))

    tris = hist[0x05] + 2 * (hist[0x06] + hist[0x07])
    print()
    print("triangles, TRI1/TRI2/QUAD only: %d (TRIN ops not counted)" % tris)
    print("matrix loads (MTX): %d, pop matrix: %d" % (hist[0xDA], hist[0xD8]))
    print("display-list jumps (DL): %d, end (ENDDL): %d" % (hist[0xDE], hist[0xDF]))
    state = sum(c for op, c in hist.items() if op not in (0x01, 0x05, 0x06, 0x07, 0x09, 0x0A, 0x0D, 0xDA, 0xD8, 0xDE, 0xDF))
    print("state and texture commands: %d" % state)
    return 0


if __name__ == "__main__":
    sys.exit(main())
