"""把 icon_preview.png 转成 ASCII 预览，验证设计。"""
import sys
sys.path.insert(0, r"D:\AiTools\deepseek\poe\cver")
from check_png import decode_png

w, h, bpp, px = decode_png(r"D:\AiTools\deepseek\poe\cver\icon_preview.png")
print(f"{w}x{h} bpp={bpp}")
print()

# 按亮度映射
CH = " .:-=+*#%@"
for y in range(0, h, 8):
    line = ""
    for x in range(0, w, 4):
        i = (y * w + x) * bpp
        r, g, b = px[i], px[i+1], px[i+2]
        a = px[i+3] if bpp == 4 else 255
        if a < 30:
            line += " "     # 真透明
            continue
        warm = (r > 100 and r > b + 30)
        lum = (r * 299 + g * 587 + b * 114) // 1000
        if warm:
            line += "@" if lum > 150 else "*"
        else:
            idx = min(len(CH) - 1, lum * len(CH) // 256)
            line += CH[idx] if idx > 0 else "."
    print(f"{y:3d}|{line}|")
