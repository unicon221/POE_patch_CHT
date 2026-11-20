/*
 * poe_ggpk.c - GGPK v3 只读访问实现
 */
#define _CRT_SECURE_NO_WARNINGS
#include "poe_ggpk.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <stdarg.h>

char poe_last_error[1024];
void poe_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(poe_last_error, sizeof poe_last_error, fmt, ap);
    va_end(ap);
}

/* ---------- 小工具 ---------- */

static int32_t rd32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t lo = (uint64_t)(uint32_t)rd32(p);
    uint64_t hi = (uint64_t)(uint32_t)rd32(p + 4);
    return lo | (hi << 32);
}

static bool seek_rd(FILE *f, int64_t off, void *buf, size_t n) {
    if (off < 0) return false;
    if (_fseeki64(f, off, SEEK_SET) != 0) return false;
    return fread(buf, 1, n, f) == n;
}

/* UTF-16LE -> UTF-8 (就地转换, 去掉结尾 NUL) */
static void u16_to_u8(const uint8_t *src, int nchars, char *dst, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < nchars && o + 4 < cap; i++) {
        uint32_t c = (uint32_t)src[i*2] | ((uint32_t)src[i*2+1] << 8);
        if (c == 0) break;
        if (c < 0x80) {
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = 0;
}

static bool ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* ---------- 记录解析 ---------- */

typedef struct {
    int64_t len;
    char    tag[5];
} rec_head;

static bool read_head(poe_ggpk *g, int64_t off, rec_head *h) {
    uint8_t b[8];
    if (!seek_rd(g->f, off, b, 8)) return false;
    h->len = (int64_t)(uint32_t)rd32(b);
    memcpy(h->tag, b + 4, 4);
    h->tag[4] = 0;
    if (h->len <= 8 || off + h->len > g->size) return false;
    return true;
}

/* 解析 PDIR, 返回名字和条目数 */
static bool read_pdir(poe_ggpk *g, int64_t off, char *name, size_t name_cap,
                      int32_t *total, int64_t *entries_off) {
    rec_head h;
    if (!read_head(g, off, &h)) return false;
    if (strcmp(h.tag, "PDIR") != 0) return false;

    uint8_t b[48];
    if (!seek_rd(g->f, off, b, 48)) return false;
    int32_t nlen = rd32(b + 8);
    int32_t tot  = rd32(b + 12);
    if (nlen < 0 || nlen > 4096 || tot < 0 || tot > 1000000) return false;

    if (name) {
        if (nlen > 0) {
            uint8_t *nb = (uint8_t*)malloc((size_t)nlen * 2);
            if (!nb) return false;
            if (!seek_rd(g->f, off + 48, nb, (size_t)nlen * 2)) { free(nb); return false; }
            u16_to_u8(nb, nlen, name, name_cap);
            free(nb);
        } else {
            name[0] = 0;
        }
    }
    if (total) *total = tot;
    if (entries_off) *entries_off = off + 48 + (int64_t)nlen * 2;
    return true;
}

/* 解析 FILE, 返回名字与数据区间 */
static bool read_file(poe_ggpk *g, int64_t off, poe_ggpk_entry *out) {
    rec_head h;
    if (!read_head(g, off, &h)) return false;
    if (strcmp(h.tag, "FILE") != 0) return false;

    uint8_t b[44];
    if (!seek_rd(g->f, off, b, 44)) return false;
    int32_t nlen = rd32(b + 8);
    if (nlen < 0 || nlen > 4096) return false;

    int64_t data_off = off + 44 + (int64_t)nlen * 2;
    if (data_off > off + h.len) return false;

    if (out) {
        out->record_offset = off;
        out->data_offset = data_off;
        out->data_length = off + h.len - data_off;
        if (nlen > 0) {
            uint8_t *nb = (uint8_t*)malloc((size_t)nlen * 2);
            if (nb) {
                if (seek_rd(g->f, off + 44, nb, (size_t)nlen * 2))
                    u16_to_u8(nb, nlen, out->name, sizeof out->name);
                else
                    out->name[0] = 0;
                free(nb);
            } else {
                out->name[0] = 0;
            }
        } else {
            out->name[0] = 0;
        }
    }
    return true;
}

/* ---------- 对外接口 ---------- */

bool poe_ggpk_open(poe_ggpk *g, const char *path) {
    memset(g, 0, sizeof *g);
    /* 以读写方式打开; 失败则退回只读 (写操作会明确报错) */
    FILE *f = fopen(path, "r+b");
    bool writable = true;
    if (!f) {
        f = fopen(path, "rb");
        writable = false;
    }
    if (!f) {
        poe_set_error("无法打开 GGPK: %s", path);
        return false;
    }
    g->writable = writable;
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    int64_t size = _ftelli64(f);
    if (size < 28) { fclose(f); poe_set_error("文件过小, 不是 GGPK"); return false; }

    uint8_t hdr[28];
    if (!seek_rd(f, 0, hdr, 28)) { fclose(f); return false; }
    if (memcmp(hdr + 4, "GGPK", 4) != 0) {
        fclose(f);
        poe_set_error("不是 GGPK 文件 (魔数不匹配)");
        return false;
    }
    uint32_t ver = (uint32_t)rd32(hdr + 8);
    int64_t root = (int64_t)rd64(hdr + 12);
    int64_t ff   = (int64_t)rd64(hdr + 20);

    if (root <= 0 || root >= size) {
        fclose(f);
        poe_set_error("GGPK 根目录偏移异常: 0x%llX", (unsigned long long)root);
        return false;
    }

    g->f = f;
    g->size = size;
    g->version = ver;
    g->root_offset = root;
    g->first_free_offset = ff;
    return true;
}

void poe_ggpk_close(poe_ggpk *g) {
    if (g->f) { fclose(g->f); g->f = NULL; }
}

bool poe_is_ggpk_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[28];
    size_t n = fread(hdr, 1, 28, f);
    fclose(f);
    if (n < 28) return false;
    return memcmp(hdr + 4, "GGPK", 4) == 0;
}

/* 在 dir 中按名字找 (大小写不敏感) */
static bool find_in_dir(poe_ggpk *g, int64_t dir_off, const char *want,
                        int64_t *found_off, char *type) {
    char name[256] = {0};
    int32_t total = 0;
    int64_t ent_off = 0;
    if (!read_pdir(g, dir_off, name, sizeof name, &total, &ent_off)) return false;

    for (int32_t i = 0; i < total; i++) {
        uint8_t e[12];
        if (!seek_rd(g->f, ent_off + (int64_t)i * 12, e, 12)) return false;
        int64_t off = (int64_t)rd64(e + 4);

        rec_head h;
        if (!read_head(g, off, &h)) continue;

        if (strcmp(h.tag, "PDIR") == 0) {
            char nm[256] = {0};
            if (read_pdir(g, off, nm, sizeof nm, NULL, NULL)) {
                if (ieq(nm, want)) {
                    if (found_off) *found_off = off;
                    if (type) *type = 'D';
                    return true;
                }
            }
        } else if (strcmp(h.tag, "FILE") == 0) {
            poe_ggpk_entry fe;
            if (read_file(g, off, &fe)) {
                if (ieq(fe.name, want)) {
                    if (found_off) *found_off = off;
                    if (type) *type = 'F';
                    return true;
                }
            }
        }
    }
    return false;
}

bool poe_ggpk_find(poe_ggpk *g, const char *path, poe_ggpk_entry *out) {
    if (!g->f) { poe_set_error("GGPK 未打开"); return false; }

    /* 逐级向下 */
    char buf[1024];
    strncpy(buf, path, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';

    int64_t cur = g->root_offset;
    char *seg = buf;
    while (*seg) {
        char *slash = strchr(seg, '/');
        if (slash) *slash = 0;

        int64_t next = 0;
        char type = 0;
        if (!find_in_dir(g, cur, seg, &next, &type)) {
            poe_set_error("GGPK 中找不到: %s", path);
            return false;
        }
        if (slash == NULL) {
            /* 最后一段 */
            if (type != 'F') {
                poe_set_error("GGPK 中的 %s 是目录, 不是文件", path);
                return false;
            }
            return read_file(g, next, out);
        }
        if (type != 'D') {
            poe_set_error("GGPK 路径中 %s 不是目录", seg);
            return false;
        }
        cur = next;
        seg = slash + 1;
    }
    poe_set_error("路径为空");
    return false;
}

uint8_t *poe_ggpk_read(poe_ggpk *g, const char *path, size_t *out_len) {
    poe_ggpk_entry e;
    if (!poe_ggpk_find(g, path, &e)) return NULL;
    if (e.data_length <= 0) {
        *out_len = 0;
        return (uint8_t*)calloc(1, 1);
    }
    uint8_t *buf = (uint8_t*)malloc((size_t)e.data_length);
    if (!buf) {
        poe_set_error("内存不足 (GGPK 读取 %lld 字节)", (long long)e.data_length);
        return NULL;
    }
    if (!seek_rd(g->f, e.data_offset, buf, (size_t)e.data_length)) {
        free(buf);
        poe_set_error("GGPK 读取失败: %s", path);
        return NULL;
    }
    *out_len = (size_t)e.data_length;
    return buf;
}

bool poe_ggpk_list(poe_ggpk *g, const char *dir_path, poe_ggpk_list_cb cb, void *user) {
    int64_t dir_off = g->root_offset;
    if (dir_path && dir_path[0]) {
        char buf[1024];
        strncpy(buf, dir_path, sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
        char *seg = buf;
        while (*seg) {
            char *slash = strchr(seg, '/');
            if (slash) *slash = 0;
            int64_t next = 0;
            char type = 0;
            if (!find_in_dir(g, dir_off, seg, &next, &type)) return false;
            if (type != 'D') return false;
            dir_off = next;
            if (!slash) break;
            seg = slash + 1;
        }
    }

    char name[256] = {0};
    int32_t total = 0;
    int64_t ent_off = 0;
    if (!read_pdir(g, dir_off, name, sizeof name, &total, &ent_off)) return false;

    for (int32_t i = 0; i < total; i++) {
        uint8_t e[12];
        if (!seek_rd(g->f, ent_off + (int64_t)i * 12, e, 12)) return false;
        int64_t off = (int64_t)rd64(e + 4);
        rec_head h;
        if (!read_head(g, off, &h)) continue;

        if (strcmp(h.tag, "PDIR") == 0) {
            char nm[256] = {0};
            if (read_pdir(g, off, nm, sizeof nm, NULL, NULL))
                cb(user, 'D', nm, 0);
        } else if (strcmp(h.tag, "FILE") == 0) {
            poe_ggpk_entry fe;
            if (read_file(g, off, &fe))
                cb(user, 'F', fe.name, fe.data_length);
        }
    }
    return true;
}


/* ============================================================ SHA256 */

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROR32(x,n) (((x) >> (n)) | ((x) << (32-(n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15],7) ^ ROR32(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = ROR32(w[i-2],17) ^ ROR32(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6]; h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e,6) ^ ROR32(e,11) ^ ROR32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = ROR32(a,2) ^ ROR32(a,13) ^ ROR32(a,22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=h;
}

static void sha256_init(sha256_ctx *c) {
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len = 0; c->buflen = 0;
}
static void sha256_update(sha256_ctx *c, const uint8_t *p, size_t n) {
    c->len += n;
    while (n > 0) {
        size_t take = 64 - c->buflen;
        if (take > n) take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; n -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
}
static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t p80 = 0x80, z = 0;
    sha256_update(c, &p80, 1);
    while (c->buflen != 56) sha256_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i*8));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]=(uint8_t)(c->h[i]>>24); out[i*4+1]=(uint8_t)(c->h[i]>>16);
        out[i*4+2]=(uint8_t)(c->h[i]>>8); out[i*4+3]=(uint8_t)(c->h[i]);
    }
}
static void sha256_buf(const uint8_t *p, size_t n, uint8_t out[32]) {
    sha256_ctx c; sha256_init(&c); sha256_update(&c, p, n); sha256_final(&c, out);
}

/* Murmur2 (GGPK 目录条目用的 nameHash) */
static uint32_t murmur2(const char *s) {
    /* LibGGPK3: 先转小写, 再 murmur2 32bit seed=0 */
    size_t len = strlen(s);
    uint8_t *lc = (uint8_t*)malloc(len + 1);
    if (!lc) return 0;
    for (size_t i = 0; i < len; i++) {
        char ch = s[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        lc[i] = (uint8_t)ch;
    }
    lc[len] = 0;

    const uint32_t m = 0x5bd1e995;
    const int r = 24;
    uint32_t h = 0 ^ (uint32_t)len;
    size_t i = 0;
    while (len - i >= 4) {
        uint32_t k;
        memcpy(&k, lc + i, 4);
        k *= m; k ^= k >> r; k *= m;
        h *= m; h ^= k;
        i += 4;
    }
    switch (len - i) {
        case 3: h ^= (uint32_t)lc[i+2] << 16; /* fallthrough */
        case 2: h ^= (uint32_t)lc[i+1] << 8;  /* fallthrough */
        case 1: h ^= (uint32_t)lc[i]; h *= m;
    }
    h ^= h >> 13; h *= m; h ^= h >> 15;
    free(lc);
    return h;
}

/* ============================================================ 写入 */

static bool wr_at(FILE *f, int64_t off, const void *buf, size_t n) {
    if (_fseeki64(f, off, SEEK_SET) != 0) return false;
    return fwrite(buf, 1, n, f) == n;
}

/* 追加数据到文件末尾, 返回写入起始偏移 */
static int64_t append_data(FILE *f, int64_t cur_size,
                           const void *buf, size_t n) {
    /* 必须先 seek 到 END 再确认实际大小, 避免 cur_size 不准 */
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        poe_set_error("append: seek END 失败 (errno=%d)", errno);
        return -1;
    }
    int64_t real_end = _ftelli64(f);
    if (real_end < 0) {
        poe_set_error("append: ftell 失败");
        return -1;
    }
    int64_t at = real_end;
    if (cur_size > real_end) at = cur_size;   /* 需要扩展时用请求值 */

    if (_fseeki64(f, at, SEEK_SET) != 0) {
        poe_set_error("append: seek 到 %lld 失败 (errno=%d)", (long long)at, errno);
        return -1;
    }
    size_t w = fwrite(buf, 1, n, f);
    if (w != n) {
        poe_set_error("append: fwrite 写入 %zu/%zu 字节失败 (errno=%d)", w, n, errno);
        return -1;
    }
    if (fflush(f) != 0) {
        poe_set_error("append: fflush 失败 (errno=%d)", errno);
        return -1;
    }
    return at;
}

typedef struct {
    uint32_t name_hash;
    int64_t  offset;
} dir_entry;

typedef struct {
    int64_t    offset;       /* PDIR 记录偏移 */
    int64_t    length;
    char       name[256];
    uint8_t    sha[32];
    dir_entry *entries;
    int        count;
    int64_t    entries_off;  /* 条目数组在原文件中的偏移 */
} pdir_info;

static bool read_pdir_full(poe_ggpk *g, int64_t off, pdir_info *o) {
    rec_head h;
    if (!read_head(g, off, &h)) return false;
    if (strcmp(h.tag, "PDIR") != 0) return false;

    memset(o, 0, sizeof *o);
    o->offset = off;
    o->length = h.len;

    uint8_t head[48];
    if (!seek_rd(g->f, off, head, 48)) return false;
    int32_t nlen = rd32(head + 8);
    int32_t total = rd32(head + 12);
    memcpy(o->sha, head + 16, 32);
    if (nlen < 0 || nlen > 1024 || total < 0 || total > 200000) return false;

    if (nlen > 0) {
        uint8_t *nb = (uint8_t*)malloc((size_t)nlen * 2);
        if (!nb) return false;
        if (!seek_rd(g->f, off + 48, nb, (size_t)nlen * 2)) { free(nb); return false; }
        u16_to_u8(nb, nlen, o->name, sizeof o->name);
        free(nb);
    }

    o->entries_off = off + 48 + (int64_t)nlen * 2;
    o->count = total;
    if (total > 0) {
        o->entries = (dir_entry*)malloc((size_t)total * sizeof(dir_entry));
        if (!o->entries) return false;
        uint8_t *eb = (uint8_t*)malloc((size_t)total * 12);
        if (!eb) { free(o->entries); o->entries = NULL; return false; }
        if (!seek_rd(g->f, o->entries_off, eb, (size_t)total * 12)) {
            free(eb); free(o->entries); o->entries = NULL; return false;
        }
        for (int i = 0; i < total; i++) {
            o->entries[i].name_hash = (uint32_t)rd32(eb + i * 12);
            o->entries[i].offset = (int64_t)rd64(eb + i * 12 + 4);
        }
        free(eb);
    }
    return true;
}

static void free_pdir(pdir_info *o) {
    if (o->entries) { free(o->entries); o->entries = NULL; }
}

/* 构造并追加一条 PDIR 记录, 返回新偏移 */
static int64_t write_pdir_record(poe_ggpk *g, const char *name,
                                 const uint8_t sha[32],
                                 const dir_entry *ents, int n, int64_t cur_size) {
    size_t name_len = strlen(name);
    size_t rec_len = 4 + 4 + 4 + 4 + 32 + name_len * 2 + 2 + (size_t)n * 12;
    uint8_t *buf = (uint8_t*)malloc(rec_len);
    if (!buf) { poe_set_error("write_pdir_record: malloc(%zu) 失败", rec_len); return -1; }

    size_t p = 0;
    uint32_t v32;
    v32 = (uint32_t)rec_len;  memcpy(buf + p, &v32, 4); p += 4;
    memcpy(buf + p, "PDIR", 4); p += 4;
    v32 = (uint32_t)(name_len + 1); memcpy(buf + p, &v32, 4); p += 4;   /* NameLength 含结尾 */
    v32 = (uint32_t)n;         memcpy(buf + p, &v32, 4); p += 4;
    memcpy(buf + p, sha, 32); p += 32;
    for (size_t i = 0; i < name_len; i++) {
        unsigned char ch = (unsigned char)name[i];
        buf[p++] = ch; buf[p++] = 0;
    }
    buf[p++] = 0; buf[p++] = 0;   /* 名字结尾 NUL */
    for (int i = 0; i < n; i++) {
        memcpy(buf + p, &ents[i].name_hash, 4); p += 4;
        memcpy(buf + p, &ents[i].offset, 8);    p += 8;
    }

    int64_t at = append_data(g->f, cur_size, buf, p);
    free(buf);
    if (at < 0) {
        char inner[512];
        strncpy(inner, poe_last_error, sizeof inner - 1);
        inner[sizeof inner - 1] = 0;
        poe_set_error("write_pdir_record(name=%s, n=%d, len=%zu) 失败: %s",
                      name, n, p, inner);
    }
    return at;
}

/* 构造并追加一条 FILE 记录, 返回新偏移 */
static int64_t write_file_record(poe_ggpk *g, const char *name,
                                 const uint8_t *data, size_t len,
                                 int64_t cur_size) {
    size_t name_len = strlen(name);
    size_t rec_len = 4 + 4 + 4 + 32 + name_len * 2 + 2 + len;
    uint8_t *buf = (uint8_t*)malloc(rec_len);
    if (!buf) return -1;

    size_t p = 0;
    uint32_t v32 = (uint32_t)rec_len;
    memcpy(buf + p, &v32, 4); p += 4;
    memcpy(buf + p, "FILE", 4); p += 4;
    v32 = (uint32_t)(name_len + 1);
    memcpy(buf + p, &v32, 4); p += 4;
    uint8_t sha[32];
    sha256_buf(data, len, sha);
    memcpy(buf + p, sha, 32); p += 32;
    for (size_t i = 0; i < name_len; i++) {
        unsigned char ch = (unsigned char)name[i];
        buf[p++] = ch; buf[p++] = 0;
    }
    buf[p++] = 0; buf[p++] = 0;
    memcpy(buf + p, data, len); p += len;

    int64_t at = append_data(g->f, cur_size, buf, p);
    free(buf);
    return at;
}

/* 在有序条目数组中插入或更新, 返回新的条目数 */
static int upsert_entry(dir_entry *ents, int n, uint32_t hash, int64_t offset) {
    int i = 0;
    while (i < n && ents[i].name_hash < hash) i++;
    if (i < n && ents[i].name_hash == hash) {
        ents[i].offset = offset;
        return n;
    }
    memmove(&ents[i + 1], &ents[i], (size_t)(n - i) * sizeof(dir_entry));
    ents[i].name_hash = hash;
    ents[i].offset = offset;
    return n + 1;
}

/* 递归写回: 从最深层目录往上, 依次重写 PDIR 并重算哈希 */
typedef struct {
    int64_t  dir_offset;   /* 当前目录记录偏移 */
    char     name[256];    /* 目录名 */
    char     child_name[256]; /* 要更新的子项名 */
} dir_path_node;

/* 前向声明 */
static int64_t write_pdir_record(poe_ggpk *g, const char *name,
                                 const uint8_t sha[32],
                                 const dir_entry *ents, int n, int64_t cur_size);
static int64_t write_file_record(poe_ggpk *g, const char *name,
                                 const uint8_t *data, size_t len, int64_t cur_size);
static int upsert_entry(dir_entry *ents, int n, uint32_t hash, int64_t offset);

/* 在指定目录下创建子目录 (若不存在), 返回其记录偏移 */
static int64_t ensure_subdir(poe_ggpk *g, int64_t parent_dir,
                             const char *name, int64_t *cur_size) {
    pdir_info pp;
    if (!read_pdir_full(g, parent_dir, &pp)) {
        poe_set_error("ensure_subdir: 读取父目录失败 @0x%llX", (unsigned long long)parent_dir);
        return -1;
    }
    uint32_t nh = murmur2(name);
    int64_t result = -1;
    for (int i = 0; i < pp.count; i++) {
        if (pp.entries[i].name_hash != nh) continue;
        rec_head h;
        if (!read_head(g, pp.entries[i].offset, &h)) continue;
        if (strcmp(h.tag, "PDIR") == 0) {
            char nm[256] = {0};
            if (read_pdir(g, pp.entries[i].offset, nm, sizeof nm, NULL, NULL) && ieq(nm, name))
                result = pp.entries[i].offset;
        }
        break;
    }
    if (result >= 0) { free_pdir(&pp); return result; }

    uint8_t zero_sha[32] = {0};
    fprintf(stderr, "[dbg] ensure_subdir: 准备写 PDIR name=%s cur_size=%lld\\n", name, (long long)*cur_size);
    int64_t nd_off = write_pdir_record(g, name, zero_sha, NULL, 0, *cur_size);
    if (nd_off < 0) {
        fprintf(stderr, "[dbg] write_pdir_record 返回 %lld, err=%s\\n", (long long)nd_off, poe_last_error);
        free_pdir(&pp); poe_set_error("ensure_subdir: 写新目录记录失败"); return -1;
    }
    size_t nl = strlen(name);
    size_t nd_len = 4 + 4 + 4 + 4 + 32 + nl * 2 + 2;
    *cur_size = nd_off + (int64_t)nd_len;

    int nc = pp.count + 1;
    dir_entry *ents = (dir_entry*)malloc((size_t)nc * sizeof(dir_entry));
    if (!ents) { free_pdir(&pp); return -1; }
    if (pp.count > 0) memcpy(ents, pp.entries, (size_t)pp.count * sizeof(dir_entry));
    nc = upsert_entry(ents, pp.count, nh, nd_off);

    uint8_t *cat = (uint8_t*)malloc((size_t)nc * 32 + 32);
    if (cat) {
        for (int i = 0; i < nc; i++) {
            rec_head sh;
            uint8_t node_sha[32] = {0};
            if (read_head(g, ents[i].offset, &sh)) {
                if (strcmp(sh.tag, "FILE") == 0) seek_rd(g->f, ents[i].offset + 12, node_sha, 32);
                else if (strcmp(sh.tag, "PDIR") == 0) seek_rd(g->f, ents[i].offset + 16, node_sha, 32);
            }
            memcpy(cat + (size_t)i * 32, node_sha, 32);
        }
        sha256_buf(cat, (size_t)nc * 32, pp.sha);
        free(cat);
    }

    int64_t np_off = write_pdir_record(g, pp.name, pp.sha, ents, nc, *cur_size);
    if (np_off < 0) { free(ents); free_pdir(&pp); poe_set_error("ensure_subdir: 重写父目录失败"); return -1; }
    size_t pnl = strlen(pp.name);
    size_t np_len = 4 + 4 + 4 + 4 + 32 + pnl * 2 + 2 + (size_t)nc * 12;
    *cur_size = np_off + (int64_t)np_len;

    if (parent_dir == g->root_offset) {
        uint8_t rb[8];
        for (int i = 0; i < 8; i++) rb[i] = (uint8_t)((uint64_t)np_off >> (i*8));
        wr_at(g->f, 12, rb, 8);
        g->root_offset = np_off;
    } else {
        pdir_info rp;
        if (read_pdir_full(g, g->root_offset, &rp)) {
            for (int k = 0; k < rp.count; k++) {
                if (rp.entries[k].offset == parent_dir) {
                    uint8_t nb[8];
                    for (int i = 0; i < 8; i++) nb[i] = (uint8_t)((uint64_t)np_off >> (i*8));
                    wr_at(g->f, rp.entries_off + (int64_t)k * 12 + 4, nb, 8);
                    break;
                }
            }
            free_pdir(&rp);
        }
    }

    free(ents);
    free_pdir(&pp);
    return nd_off;
}
bool poe_ggpk_write_file(poe_ggpk *g, const char *path,
                         const uint8_t *data, size_t len) {
    if (!g->f) { poe_set_error("GGPK 未打开"); return false; }
    if (!g->writable) {
        poe_set_error("GGPK 文件以只读方式打开，无法写入。"
                      "请确认：1) 游戏已完全关闭 2) 有文件写入权限（可尝试以管理员身份运行）");
        return false;
    }

    /* 拆分路径 */
    char buf[1024];
    strncpy(buf, path, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';

    char *segs[32];
    int nseg = 0;
    char *s = buf;
    while (*s && nseg < 32) {
        segs[nseg++] = s;
        char *slash = strchr(s, '/');
        if (!slash) break;
        *slash = 0;
        s = slash + 1;
    }
    if (nseg < 2) { poe_set_error("路径至少需要一个目录和一个文件名"); return false; }

    /* 逐级解析到父目录 */
    int64_t cur_dir = g->root_offset;
    for (int i = 0; i < nseg - 1; i++) {
        char nm[256] = {0};
        int32_t total = 0;
        int64_t ent_off = 0;
        if (!read_pdir(g, cur_dir, nm, sizeof nm, &total, &ent_off)) {
            poe_set_error("读取目录失败: %s", segs[i]);
            return false;
        }
        bool found = false;
        for (int32_t k = 0; k < total; k++) {
            uint8_t e[12];
            if (!seek_rd(g->f, ent_off + (int64_t)k * 12, e, 12)) break;
            int64_t off = (int64_t)rd64(e + 4);
            rec_head h;
            if (!read_head(g, off, &h)) continue;
            if (strcmp(h.tag, "PDIR") != 0) continue;
            char sub[256] = {0};
            if (read_pdir(g, off, sub, sizeof sub, NULL, NULL) && ieq(sub, segs[i])) {
                cur_dir = off;
                found = true;
                break;
            }
        }
        if (!found) {
            /* 自动创建缺失的中间目录 */
            int64_t cur_size_tmp = g->size;
            int64_t nd = ensure_subdir(g, cur_dir, segs[i], &cur_size_tmp);
            if (nd < 0) {
                return false;   /* 保留 ensure_subdir 设置的错误信息 */
            }
            g->size = cur_size_tmp;
            /* 更新 GGPK 头部长度 */
            {
                uint8_t szb[4];
                szb[0] = (uint8_t)(cur_size_tmp & 0xFF);
                szb[1] = (uint8_t)((cur_size_tmp >> 8) & 0xFF);
                szb[2] = (uint8_t)((cur_size_tmp >> 16) & 0xFF);
                szb[3] = (uint8_t)((cur_size_tmp >> 24) & 0xFF);
                wr_at(g->f, 0, szb, 4);
            }
            cur_dir = nd;
        }
    }

    /* 当前目录信息 */
    pdir_info pd;
    if (!read_pdir_full(g, cur_dir, &pd)) {
        poe_set_error("读取父目录失败");
        return false;
    }

    int64_t cur_size = g->size;
    const char *fname = segs[nseg - 1];
    uint32_t fhash = murmur2(fname);

    /* 1. 写新的 FILE 记录 */
    int64_t new_file_off = write_file_record(g, fname, data, len, cur_size);
    if (new_file_off < 0) { free_pdir(&pd); poe_set_error("追加 FILE 记录失败"); return false; }
    /* 计算该记录的实际长度 */
    size_t name_len = strlen(fname);
    size_t rec_len = 4 + 4 + 4 + 32 + name_len * 2 + 2 + len;
    cur_size = new_file_off + (int64_t)rec_len;

    /* 2. 更新条目表 */
    int new_count = pd.count;
    dir_entry *ents = (dir_entry*)malloc(((size_t)pd.count + 1) * sizeof(dir_entry));
    if (!ents) { free_pdir(&pd); poe_set_error("内存不足"); return false; }
    if (pd.count > 0) memcpy(ents, pd.entries, (size_t)pd.count * sizeof(dir_entry));
    new_count = upsert_entry(ents, pd.count, fhash, new_file_off);

    /* 3. 重算目录哈希: 子项哈希串接后 SHA256 */
    uint8_t *cat = (uint8_t*)malloc((size_t)new_count * 32 + 32);
    if (cat) {
        for (int i = 0; i < new_count; i++) {
            rec_head sh;
            if (read_head(g, ents[i].offset, &sh)) {
                uint8_t node_sha[32];
                if (strcmp(sh.tag, "FILE") == 0) {
                    seek_rd(g->f, ents[i].offset + 12, node_sha, 32);
                } else if (strcmp(sh.tag, "PDIR") == 0) {
                    seek_rd(g->f, ents[i].offset + 16, node_sha, 32);
                } else {
                    memset(node_sha, 0, 32);
                }
                memcpy(cat + (size_t)i * 32, node_sha, 32);
            } else {
                memset(cat + (size_t)i * 32, 0, 32);
            }
        }
        sha256_buf(cat, (size_t)new_count * 32, pd.sha);
        free(cat);
    }

    /* 4. 追加新的 PDIR 记录 */
    int64_t new_pdir_off = write_pdir_record(g, pd.name, pd.sha, ents, new_count, cur_size);
    if (new_pdir_off < 0) {
        free(ents); free_pdir(&pd);
        poe_set_error("追加 PDIR 记录失败");
        return false;
    }
    size_t pd_name_len = strlen(pd.name);
    size_t pd_rec_len = 4 + 4 + 4 + 4 + 32 + pd_name_len * 2 + 2 + (size_t)new_count * 12;
    cur_size = new_pdir_off + (int64_t)pd_rec_len;

    /* 5. 更新父目录里指向本目录的 offset */
    /* 需要找到父目录 (nseg-2 层) —— 简化: 从 root 重新走到父层 */
    int64_t parent_dir = g->root_offset;
    for (int i = 0; i < nseg - 2; i++) {
        char nm[256] = {0};
        int32_t total = 0;
        int64_t ent_off = 0;
        if (!read_pdir(g, parent_dir, nm, sizeof nm, &total, &ent_off)) break;
        bool found = false;
        for (int32_t k = 0; k < total; k++) {
            uint8_t e[12];
            if (!seek_rd(g->f, ent_off + (int64_t)k * 12, e, 12)) break;
            int64_t off = (int64_t)rd64(e + 4);
            rec_head h;
            if (!read_head(g, off, &h)) continue;
            if (strcmp(h.tag, "PDIR") != 0) continue;
            char sub[256] = {0};
            if (read_pdir(g, off, sub, sizeof sub, NULL, NULL) && ieq(sub, segs[i])) {
                parent_dir = off;
                found = true;
                break;
            }
        }
        if (!found) break;
    }
    {
        char nm[256] = {0};
        int32_t total = 0;
        int64_t ent_off = 0;
        if (read_pdir(g, parent_dir, nm, sizeof nm, &total, &ent_off)) {
            for (int32_t k = 0; k < total; k++) {
                uint8_t e[12];
                if (!seek_rd(g->f, ent_off + (int64_t)k * 12, e, 12)) break;
                int64_t off = (int64_t)rd64(e + 4);
                if (off == pd.offset) {
                    uint8_t nb[8];
                    for (int i = 0; i < 8; i++) nb[i] = (uint8_t)((uint64_t)new_pdir_off >> (i*8));
                    wr_at(g->f, ent_off + (int64_t)k * 12 + 4, nb, 8);
                    break;
                }
            }
        }
    }

    /* 6. 更新 GGPK 头部长度 */
    {
        uint8_t szb[4];
        szb[0] = (uint8_t)(cur_size & 0xFF);
        szb[1] = (uint8_t)((cur_size >> 8) & 0xFF);
        szb[2] = (uint8_t)((cur_size >> 16) & 0xFF);
        szb[3] = (uint8_t)((cur_size >> 24) & 0xFF);
        wr_at(g->f, 0, szb, 4);
    }
    fflush(g->f);

    free(ents);
    free_pdir(&pd);
    g->size = cur_size;
    return true;
}

bool poe_ggpk_verify(const char *ggpk_path, const char *path,
                     const uint8_t *expect, size_t expect_len) {
    poe_ggpk g;
    if (!poe_ggpk_open(&g, (char*)ggpk_path)) return false;
    size_t got_len = 0;
    uint8_t *got = poe_ggpk_read(&g, path, &got_len);
    poe_ggpk_close(&g);
    if (!got) return false;
    bool ok = (got_len == expect_len) && (memcmp(got, expect, expect_len) == 0);
    free(got);
    return ok;
}