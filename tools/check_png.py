"""完整解码 PNG 并统计，验证 UI 渲染是否正常。"""
import struct
import zlib
import collections


def decode_png(path):
    f = open(path, "rb").read()
    w, h = struct.unpack(">II", f[16:24])
    bit_depth, color_type = f[24], f[25]
    pos = 8
    idat = b""
    while pos < len(f):
        ln = struct.unpack(">I", f[pos:pos+4])[0]
        typ = f[pos+4:pos+8]
        if typ == b"IDAT":
            idat += f[pos+8:pos+8+ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    bpp = 4 if color_type == 6 else 3
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]; p += 1
        row = bytearray(raw[p:p+stride]); p += stride
        if ft == 1:
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i-bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = row[i-bpp] if i >= bpp else 0
                row[i] = (row[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = row[i-bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i-bpp] if i >= bpp else 0
                pp = a + b - c
                pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        out[y*stride:(y+1)*stride] = row
        prev = row
    return w, h, bpp, bytes(out)


w, h, bpp, px = decode_png(r"D:\AiTools\deepseek\poe\cver\ui.png")
print(f"尺寸: {w}x{h}, bpp={bpp}")
cnt = collections.Counter()
for y in range(h):
    for x in range(0, w, 3):
        i = (y*w + x) * bpp
        cnt[px[i:i+3]] += 1
print("唯一颜色:", len(cnt))
print("最常见 8 种:")
for c, n in cnt.most_common(8):
    print(f"  #{c.hex()}  {n}")
total = sum(cnt.values())
black = cnt.get(b'\x00\x00\x00', 0)
white = cnt.get(b'\xff\xff\xff', 0)
print(f"\n黑色占比: {black/total*100:.1f}%")
print(f"白色占比: {white/total*100:.1f}%")
