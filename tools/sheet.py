#!/usr/bin/env python3
"""Contact sheet of headless frame dumps: sheet.py out.png cols f1.ppm f2.ppm ..."""
import sys
from PIL import Image, ImageDraw
out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
ims = [Image.open(f) for f in files]
w, h = ims[0].size
rows = (len(ims) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (w + 2), rows * (h + 12)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, (im, f) in enumerate(zip(ims, files)):
    x, y = (i % cols) * (w + 2), (i // cols) * (h + 12)
    sheet.paste(im, (x, y + 11))
    d.text((x + 2, y), f.split("/")[-1].replace(".ppm", ""), fill=(255, 255, 0))
sheet.save(out)
