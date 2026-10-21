# Makes the game's splash and loading screen picture from a portrait render.
#
#   Build\Game\Windows\RigExecBiped.exe /Game/RigExec/L_RigExecGame -RigExecPortrait -UseFixedTimeStep -FPS=30 -windowed -resx=1280 -resy=720
#   python Scripts/make_splash.py Build/Game/Windows/RigExecBiped/Saved/RigExecPortrait/portrait_0.png Examples/RigExecBiped/Content/Splash
#
# Writes LoadingScreen.png (1920x1080, the in-game loading screen) and
# Splash.bmp (960x540, the window Windows shows while the engine starts).
# Needs Pillow and the Bahnschrift font (Windows).
import sys, math
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageChops
src, out = sys.argv[1], sys.argv[2]
W, H = 1920, 1080
F = "C:/Windows/Fonts/"
def font(name, size): return ImageFont.truetype(F + name, size)

# Backdrop: deep navy, lighter toward the face.
bg = Image.new("RGB", (W, H))
px = bg.load()
for y in range(H):
    for x in range(W):
        d = math.hypot((x - 1330) / W, (y - 300) / H)
        k = max(0.0, 1.0 - d * 1.6)
        px[x, y] = (int(8 + 22 * k), int(12 + 34 * k), int(22 + 52 * k))

# The portrait (a -RigExecPortrait render), face on the right, its
# background graded down into the backdrop.
frame = Image.open(src).convert("RGB")
SC = 0.66
fw, fh = int(frame.width * SC), int(frame.height * SC)
face = frame.resize((fw, fh), Image.LANCZOS)
fx = 1330 - int(1375 * SC)
mask = Image.new("L", (fw, fh), 0)
mp = mask.load()
fcx, fcy = int(1375 * SC), int(330 * SC)
for y in range(0, fh, 2):
    for x in range(0, fw, 2):
        a = min(1.0, max(0.0, (x - 40) / 420.0)) ** 1.5      # fade in from the left
        d = math.hypot((x - fcx) / 560.0, (y - fcy) / 520.0)  # keep the head, sink the rest
        k = max(0.0, min(1.0, 1.25 - d * 0.75))
        b = min(1.0, max(0.0, (fh - y) / 300.0))
        v = int(255 * a * b * (0.3 + 0.7 * k))
        mp[x, y] = v
        if x + 1 < fw: mp[x + 1, y] = v
        if y + 1 < fh:
            mp[x, y + 1] = v
            if x + 1 < fw: mp[x + 1, y + 1] = v
bg.paste(face, (fx, 0), mask.filter(ImageFilter.GaussianBlur(6)))

# The pad's glowing ring, behind the title.
glow = Image.new("RGB", (W, H))
gd = ImageDraw.Draw(glow)
cx, cy, rx, ry = 480, 735, 330, 78
gd.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), outline=(60, 190, 255), width=8)
gd.ellipse((cx - rx * 0.86, cy - ry * 0.86, cx + rx * 0.86, cy + ry * 0.86), outline=(40, 120, 200), width=2)
for k in range(1, 5):
    f = 0.86 - k * 0.17
    gd.ellipse((cx - rx * f, cy - ry * f, cx + rx * f, cy + ry * f), outline=(18, 60 - k * 8, 110 - k * 15), width=2)
# Light rising off the pad.
beam = Image.new("RGB", (W, H))
bd = ImageDraw.Draw(beam)
for y in range(0, 340):
    a = (1 - y / 340) ** 2.5
    w = rx * 0.82 * (1 - y / 1100)
    bd.line((cx - w, cy - y, cx + w, cy - y), fill=(int(14 * a), int(70 * a), int(130 * a)))
bd.ellipse((cx - rx * 0.9, cy - ry * 0.9, cx + rx * 0.9, cy + ry * 0.9), fill=(10, 50, 95))
beam = beam.filter(ImageFilter.GaussianBlur(30))
bloom = glow.filter(ImageFilter.GaussianBlur(28))
bloom2 = glow.filter(ImageFilter.GaussianBlur(6))
bg = ImageChops.add(bg, bloom)
bg = ImageChops.add(bg, bloom)
bg = ImageChops.add(bg, bloom2)
bg = ImageChops.add(bg, glow.point(lambda v: v // 2))
bg = ImageChops.add(bg, beam)

d = ImageDraw.Draw(bg)
def spaced(text, xy, f, fill, track):
    x, y = xy
    for ch in text:
        d.text((x, y), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + track
    return x
title = font("bahnschrift.ttf", 150)
tw = d.textlength("RigExec", font=title)
# Title shadow, then title.
sh = Image.new("L", (W, H), 0)
ImageDraw.Draw(sh).text((cx - tw / 2 + 6, 420 + 10), "RigExec", font=title, fill=200)
bg.paste((0, 0, 0), (0, 0), sh.filter(ImageFilter.GaussianBlur(14)))
d = ImageDraw.Draw(bg)
d.text((cx - tw / 2, 420), "RigExec", font=title, fill=(240, 248, 255))
sub = font("bahnschrift.ttf", 54)
track = 26
sw = sum(d.textlength(c, font=sub) + track for c in "BIPED") - track
spaced("BIPED", (cx - sw / 2, 590), sub, (90, 205, 255), track)
small = font("bahnschrift.ttf", 26)
tag = "usdRig  \u00b7  Squarebit Subdivs  \u00b7  Squarebit Eye"
d.text((cx - d.textlength(tag, font=small) / 2, 850), tag, font=small, fill=(170, 195, 215))

# Corner marks and credit.
cr = font("bahnschrift.ttf", 22)
d.text((W - 110 - d.textlength("SQUAREBIT", font=cr), H - 74), "SQUAREBIT", font=cr, fill=(150, 175, 195))
for (x, y, sx, sy) in ((40, 40, 1, 1), (W - 40, 40, -1, 1), (40, H - 40, 1, -1), (W - 40, H - 40, -1, -1)):
    d.line((x, y, x + 46 * sx, y), fill=(70, 160, 220), width=3)
    d.line((x, y, x, y + 46 * sy), fill=(70, 160, 220), width=3)

bg.save(out + "/LoadingScreen.png")
small_splash = bg.resize((960, 540), Image.LANCZOS)
small_splash.save(out + "/Splash.bmp")
print("done")
