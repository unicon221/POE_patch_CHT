"""验证 exe 中是否嵌入了图标资源。"""
import struct
import sys


def list_resources(path):
    d = open(path, "rb").read()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    if d[e:e+4] != b"PE\0\0":
        raise ValueError("not PE")
    coff = e + 4
    nsec = struct.unpack_from("<H", d, coff + 2)[0]
    opt_size = struct.unpack_from("<H", d, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", d, opt)[0]
    plus = magic == 0x20b
    dd = opt + (112 if plus else 96)
    rsrc_rva, rsrc_size = struct.unpack_from("<II", d, dd + 16)

    if rsrc_rva == 0:
        return [], None

    sec_off = opt + opt_size
    secs = []
    for i in range(nsec):
        b = sec_off + i * 40
        name = d[b:b+8].rstrip(b"\0").decode("latin1")
        vsize, vaddr, rsize, raddr = struct.unpack_from("<IIII", d, b + 8)
        secs.append((name, vaddr, vsize, raddr, rsize))

    def r2o(rva):
        for name, vaddr, vsize, raddr, rsize in secs:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raddr + (rva - vaddr)
        return None

    base = r2o(rsrc_rva)
    found = []

    # 遍历资源目录树
    def walk(off, depth, path_names):
        n_named, n_id = struct.unpack_from("<HH", d, base + off + 12)
        total = n_named + n_id
        for i in range(total):
            ent = base + off + 16 + i * 8
            name_id, target = struct.unpack_from("<II", d, ent)
            if name_id & 0x80000000:
                # 字符串名
                no = base + (name_id & 0x7FFFFFFF)
                nlen = struct.unpack_from("<H", d, no)[0]
                nm = d[no+2:no+2+nlen*2].decode("utf-16-le")
            else:
                nm = name_id
            if target & 0x80000000:
                walk(target & 0x7FFFFFFF, depth + 1, path_names + [nm])
            else:
                data_ent = base + target
                rva, size, cp, _ = struct.unpack_from("<IIII", d, data_ent)
                found.append((path_names + [nm], rva, size))
    walk(0, 0, [])
    return found, (rsrc_rva, rsrc_size)


res, info = list_resources(sys.argv[1])
RT = {1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG",
      6: "STRING", 9: "ACCELERATOR", 10: "RCDATA", 12: "GROUP_CURSOR",
      14: "GROUP_ICON", 16: "VERSION", 24: "MANIFEST"}
print(f"资源节: rva=0x{info[0]:X} size={info[1]}")
print(f"资源条目: {len(res)}")
groups = {}
for path, rva, size in res:
    t = path[0]
    tn = RT.get(t, str(t))
    groups.setdefault(tn, []).append((path, rva, size))

for tn, items in groups.items():
    print(f"\n[{tn}] {len(items)} 个")
    for path, rva, size in items[:12]:
        print(f"   路径={path}  rva=0x{rva:X}  size={size}")
