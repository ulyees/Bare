"""生成程序图标：圆角蓝底 + 三条文本横线（极简、扁平、现代）。"""
from PIL import Image, ImageDraw

SIZES = [16, 24, 32, 48, 64, 128, 256]
S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((8, 8, 248, 248), radius=54, fill=(47, 107, 255, 255))
for x0, y, x1 in [(62, 78, 194), (62, 124, 194), (62, 170, 138)]:
    d.rounded_rectangle((x0, y, x1, y + 18), radius=9, fill=(255, 255, 255, 255))
img.save("app.ico", format="ICO", sizes=[(s, s) for s in SIZES])
print("app.ico 生成完成，尺寸:", SIZES)
