"""
生成 Windows .ico 图标（纯 Python，无第三方依赖）

设计: 深色圆角方块 + 橙色 "P" 字形 + 底部强调条
配色参考 Path of Exile 的暗金/琥珀风格
"""

import struct
import zlib


def blend(c1, c2, t):
    """线性插值两个 RGB"""
    return tuple(int(a + (b - a) * t) for a, b in zip(c1, c2))


def make_rgba(size):
    """生成指定尺寸的 RGBA 像素数组 (返回 bytes, 逐行从上到下)"""
    S = size
    px = bytearray(S * S * 4)

    # 颜色定义
    BG_TOP = (38, 30, 24)      # 深褐
    BG_BOT = (22, 17, 13)      # 更深的褐
    ACCENT = (200, 148, 62)    # 暗金
    ACCENT_HI = (245, 198, 108)  # 亮金
    EDGE = (58, 46, 34)        # 边框

    r = S * 0.22  # 圆角半径

    def inside_rounded(x, y):
        """圆角矩形内部判定 (点到内缩矩形的距离 <= r)"""
        if 0 <= x < S and 0 <= y < S:
            # 到最近"内部直线段"的水平/垂直距离
            dx = 0.0
            dy = 0.0
            if x < r:
                dx = r - x
            elif x > S - r:
                dx = x - (S - r)
            if y < r:
                dy = r - y
            elif y > S - r:
                dy = y - (S - r)
            return dx * dx + dy * dy <= r * r
        return False

    for y in range(S):
        for x in range(S):
            i = (y * S + x) * 4
            # 超采样抗锯齿 (3x3)
            cover = 0
            sub = 3
            for sy in range(sub):
                for sx in range(sub):
                    fx = x + (sx + 0.5) / sub
                    fy = y + (sy + 0.5) / sub
                    if inside_rounded(fx, fy):
                        cover += 1
            cover /= (sub * sub)

            if cover <= 0:
                px[i:i+4] = b'\x00\x00\x00\x00'
                continue

            # 背景竖直渐变
            t = y / max(1, S - 1)
            col = blend(BG_TOP, BG_BOT, t)

            # 边框: 判断是否落在内缩 border_w 的圆角矩形之外
            border_w = max(1.0, S * 0.04)
            rin = r - border_w * 0.5
            # 内缩后的点
            fx = x + 0.5
            fy = y + 0.5
            dxi = 0.0
            dyi = 0.0
            if fx < r:
                dxi = r - fx
            elif fx > S - r:
                dxi = fx - (S - r)
            if fy < r:
                dyi = r - fy
            elif fy > S - r:
                dyi = fy - (S - r)
            dist_edge = max(abs(dxi), abs(dyi))   # 0 在中心, r 在边缘
            # 直边距离
            straight = min(fx, fy, S - fx, S - fy)
            on_border = (dist_edge > r - border_w) or (straight < border_w)
            if on_border:
                col = blend(col, EDGE, 0.8)

            px[i:i+4] = bytes([col[0], col[1], col[2], int(255 * cover)])

    # ---- 画字母 "P" ----
    # 布局参数 (归一化)
    stem_x0 = 0.32 * S
    stem_x1 = 0.44 * S
    stem_y0 = 0.22 * S
    stem_y1 = 0.78 * S

    bowl_x1 = 0.70 * S          # 弯的右边界
    bowl_y0 = 0.22 * S
    bowl_y1 = 0.52 * S          # 弯的下沿
    bowl_mid = (bowl_y0 + bowl_y1) * 0.5
    bowl_ry = (bowl_y1 - bowl_y0) * 0.5
    bowl_thick = 0.12 * S

    for y in range(S):
        fy = y + 0.5
        for x in range(S):
            i = (y * S + x) * 4
            if px[i+3] == 0:
                continue
            fx = x + 0.5
            on = False

            # 竖笔
            if stem_x0 <= fx < stem_x1 and stem_y0 <= fy < stem_y1:
                on = True

            # 弯: 外椭圆 - 内椭圆 (上半部分)
            if not on:
                if bowl_y0 <= fy <= bowl_y1:
                    cx = stem_x1                      # 弯的圆心 x 起点
                    rx = bowl_x1 - stem_x1
                    ry = bowl_ry
                    cy = bowl_mid
                    if rx > 0 and ry > 0:
                        dx = (fx - cx) / rx
                        dy = (fy - cy) / ry
                        outer = dx * dx + dy * dy <= 1.0
                        # 内椭圆 (厚度)
                        rx2 = rx - bowl_thick
                        ry2 = ry - bowl_thick
                        inner = False
                        if rx2 > 0 and ry2 > 0:
                            dx2 = (fx - cx) / rx2
                            dy2 = (fy - cy) / ry2
                            inner = dx2 * dx2 + dy2 * dy2 <= 1.0
                        if outer and not inner:
                            on = True

            if on:
                tt = (fy - stem_y0) / max(1.0, stem_y1 - stem_y0)
                c = blend(ACCENT_HI, ACCENT, tt)
                px[i:i+4] = bytes([c[0], c[1], c[2], 255])

    # ---- 底部强调条 ----
    sy0 = int(S * 0.865)
    sy1 = int(S * 0.905)
    sx0 = int(S * 0.28)
    sx1 = int(S * 0.72)
    for y in range(sy0, min(sy1, S)):
        for x in range(sx0, min(sx1, S)):
            i = (y * S + x) * 4
            if px[i+3] == 0:
                continue
            c = ACCENT
            px[i:i+4] = bytes([c[0], c[1], c[2], 255])

    return bytes(px)


def png_bytes(size, rgba):
    """编码为 PNG"""
    def chunk(typ, data):
        c = struct.pack(">I", len(data)) + typ + data
        c += struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        return c

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8bit RGBA
    # 每行前加 filter byte 0
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        raw += rgba[y*size*4:(y+1)*size*4]

    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", ihdr)
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    return out


def ico_bytes(images):
    """images: [(size, png_bytes)] -> .ico"""
    n = len(images)
    header = struct.pack("<HHH", 0, 1, n)
    entries = b""
    data = b""
    offset = 6 + 16 * n
    for size, png in images:
        w = 0 if size >= 256 else size
        h = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII",
                               w, h, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
        data += png
    return header + entries + data


def main():
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = []
    for s in sizes:
        rgba = make_rgba(s)
        images.append((s, png_bytes(s, rgba)))
        print(f"  {s}x{s} OK")

    ico = ico_bytes(images)
    open("app.ico", "wb").write(ico)
    print(f"app.ico: {len(ico)} 字节")

    # 另存一张 256 预览 PNG
    rgba256 = make_rgba(256)
    open("icon_preview.png", "wb").write(png_bytes(256, rgba256))
    print("icon_preview.png OK")


if __name__ == "__main__":
    main()
