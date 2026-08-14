# Generates resources/app.ico (multi-size) + a PNG preview.
from PIL import Image, ImageDraw

SIZE = 256
OUT_ICO = r"C:\Coding\Work\work4-tools\resources\app.ico"
OUT_PNG = r"C:\Coding\Work\work4-tools\tools\app-icon-preview.png"


def lerp(c1, c2, t):
    return tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(3))


# --- background: diagonal blue -> violet gradient, rounded square ---
bg = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
c1 = (59, 130, 246)   # #3B82F6
c2 = (139, 92, 246)   # #8B5CF6
for y in range(SIZE):
    for x in range(SIZE):
        t = (x + y) / (2 * (SIZE - 1))
        bg.putpixel((x, y), lerp(c1, c2, t) + (255,))

mask = Image.new("L", (SIZE, SIZE), 0)
ImageDraw.Draw(mask).rounded_rectangle([10, 10, SIZE - 11, SIZE - 11],
                                       radius=52, fill=255)
transparent = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
bg = Image.composite(bg, transparent, mask)
d = ImageDraw.Draw(bg)
d.rounded_rectangle([10, 10, SIZE - 11, SIZE - 11], radius=52,
                    outline=(255, 255, 255, 70), width=3)

# --- folder: white with a soft drop shadow ---
shadow = (15, 23, 42, 42)
d.rounded_rectangle([40, 96, 192, 208], radius=14, fill=shadow)   # body shadow
d.rounded_rectangle([40, 92, 136, 124], radius=12, fill=shadow)   # tab shadow
d.rounded_rectangle([36, 92, 188, 204], radius=14, fill=(255, 255, 255, 255))
d.rounded_rectangle([36, 88, 132, 120], radius=12, fill=(255, 255, 255, 255))

# --- magnifier: cyan ring + handle, overlapping the folder ---
cyan = (34, 211, 238)
# handle first (ends get round caps)
d.line([(188, 198), (220, 230)], fill=cyan, width=22)
d.ellipse([176, 186, 200, 210], fill=cyan)
d.ellipse([212, 222, 236, 246], fill=cyan)
# ring
d.ellipse([112, 122, 204, 214], fill=cyan)
# lens glass
d.ellipse([126, 136, 190, 200], fill=(190, 245, 255, 205))
# lens highlight
d.ellipse([138, 148, 166, 176], fill=(255, 255, 255, 130))

# --- save ---
ico_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
             (128, 128), (256, 256)]
bg.save(OUT_ICO, format="ICO", sizes=ico_sizes)
bg.save(OUT_PNG, format="PNG")
print("saved", OUT_ICO)
print("saved", OUT_PNG)

chk = Image.open(OUT_ICO)
print("ico sizes:", sorted(chk.ico.sizes()))
