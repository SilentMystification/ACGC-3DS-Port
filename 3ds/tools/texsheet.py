"""texsheet.py - contact sheet of textures dumped by the 3DS build (debug switch "dumptex").

Usage: python 3ds/tools/texsheet.py [dump_dir] [out.png]
Default dump_dir: %APPDATA%/Azahar/sdmc/3ds/AnimalCrossing/texdump
Each tile shows the texture on a checkerboard (so alpha is visible) with its index.
"""
import os
import re
import sys

from PIL import Image, ImageDraw

src = sys.argv[1] if len(sys.argv) > 1 else os.path.expandvars(r"%APPDATA%/Azahar/sdmc/3ds/AnimalCrossing/texdump")
out = sys.argv[2] if len(sys.argv) > 2 else "build3ds/texsheet.png"
TILE, COLS = 72, 16

files = sorted(f for f in os.listdir(src) if f.endswith(".rgba"))
rows = (len(files) + COLS - 1) // COLS
sheet = Image.new("RGB", (COLS * TILE, rows * TILE), (40, 40, 40))
draw = ImageDraw.Draw(sheet)
for i, name in enumerate(files):
    m = re.match(r"(\d+)_(\d+)x(\d+)_(\d+)\.rgba", name)
    idx, w, h, fmt = (int(x) for x in m.groups())
    img = Image.frombytes("RGBA", (w, h), open(os.path.join(src, name), "rb").read())
    img.thumbnail((TILE - 4, TILE - 14))
    checker = Image.new("RGBA", img.size, (128, 128, 128, 255))
    cd = ImageDraw.Draw(checker)
    for y in range(0, img.size[1], 4):
        for x in range(0, img.size[0], 4):
            if (x // 4 + y // 4) % 2:
                cd.rectangle([x, y, x + 3, y + 3], fill=(192, 192, 192, 255))
    checker.alpha_composite(img)
    x0, y0 = (i % COLS) * TILE, (i // COLS) * TILE
    sheet.paste(checker.convert("RGB"), (x0 + 2, y0 + 12))
    draw.text((x0 + 2, y0), f"{idx} {w}x{h} f{fmt}", fill=(255, 255, 0))
sheet.save(out)
print(f"{len(files)} textures -> {out}")
