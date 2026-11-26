#!/usr/bin/env python3
"""psw-dashboard.ico - PSWloader Dashboard icon in the style of the other PSWloader icons: blue circle (#3A86E0),
dark symbol (#1B1B1B): a gauge with three bars. Sizes 16..256, drawn at 8x and scaled down."""
from PIL import Image, ImageDraw
BLUE, DARK, S = (58, 134, 224, 255), (27, 27, 27, 255), 2048
im = Image.new('RGBA', (S, S), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
d.ellipse((40, 40, S - 40, S - 40), fill=BLUE)
w = 150                                   # stroke width
# frame of a window/screen
d.rounded_rectangle((470, 560, 1578, 1400), radius=110, outline=DARK, width=w)
# three bars (graph) inside
for x, top in ((700, 1050), (960, 860), (1220, 960)):
    d.rounded_rectangle((x, top, x + 130, 1230), radius=60, fill=DARK)
# stand
d.rounded_rectangle((900, 1400, 1148, 1560), radius=0, fill=DARK)
d.rounded_rectangle((720, 1520, 1328, 1630), radius=55, fill=DARK)
sizes = [256, 128, 64, 48, 32, 24, 16]
im.resize((256, 256), Image.LANCZOS).save('psw-dashboard.ico', sizes=[(s, s) for s in sizes])
im.resize((128, 128), Image.LANCZOS).save('psw-dashboard-preview.png')
