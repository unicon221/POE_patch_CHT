/*
 * poe_core.c - Path of Exile Bundles2 读写核心
 */
#define _CRT_SECURE_NO_WARNINGS
#include "poe_core.h"
#include "poe_ggpk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  define DLLOPEN(p) ((void*)LoadLibraryA(p))
#  define DLLSYM(h,n) ((void*)GetProcAddress((HMODULE)(h), n))
#else
#  include <dlfcn.h>
#  define DLLOPEN(p) dlopen(p, RTLD_NOW)
#  define DLLSYM(h,n) dlsym(h, n)
#endif

/* poe_last_error / poe_set_error 定义在 poe_ggpk.c (作为公共基础设施) */

/* ============================================================ Oodle */

typedef struct {
    void *handle;
    void *p_decompress;
    void *p_compress;
    void *p_dec_mem_needed;
    void *p_cbuf_needed;
    void *p_scratch_bound;
    void *decoder_mem;
    size_t decoder_len;
    void *scratch_mem;
    size_t scratch_len;
    bool ready;
} oodle_ctx;

static oodle_ctx g_oodle;

typedef long long (*fn_dec)(const void*, long long, void*, long long,
                            int, int, int, void*, long long,
                            void*, void*, void*, long long, int);
typedef long long (*fn_comp)(int, const void*, long long, void*, int,
                             void*, void*, void*, void*, long long);
typedef int  (*fn_dec_size)(int, long long);
typedef long long (*fn_cbuf)(int, long long);
typedef long long (*fn_scratch)(int, int, long long, void*);

static bool oodle_bind(void *handle) {
    g_oodle.p_decompress     = DLLSYM(handle, "OodleLZ_Decompress");
    g_oodle.p_compress       = DLLSYM(handle, "OodleLZ_Compress");
    g_oodle.p_dec_mem_needed = DLLSYM(handle, "OodleLZDecoder_MemorySizeNeeded");
    g_oodle.p_cbuf_needed    = DLLSYM(handle, "OodleLZ_GetCompressedBufferSizeNeeded");
    g_oodle.p_scratch_bound  = DLLSYM(handle, "OodleLZ_GetCompressScratchMemBound");
    if (!g_oodle.p_decompress || !g_oodle.p_compress ||
        !g_oodle.p_dec_mem_needed || !g_oodle.p_cbuf_needed ||
        !g_oodle.p_scratch_bound) {
        poe_set_error("oo2core.dll 缺少必需的导出函数");
        return false;
    }
    int n = ((fn_dec_size)g_oodle.p_dec_mem_needed)(-1, POE_CHUNK_SIZE);
    if (n <= 0) n = 6 * 1024 * 1024;
    g_oodle.decoder_mem = malloc((size_t)n);
    if (!g_oodle.decoder_mem) { poe_set_error("内存不足 (decoder)"); return false; }
    g_oodle.decoder_len = (size_t)n;
    g_oodle.handle = handle;
    g_oodle.ready = true;
    return true;
}

bool poe_oodle_init(const char *dll_path) {
    if (g_oodle.ready) return true;
    void *h = DLLOPEN(dll_path);
    if (!h) {
        poe_set_error("无法加载 oo2core.dll: %s", dll_path);
        return false;
    }
    return oodle_bind(h);
}

void poe_oodle_shutdown(void) {
    if (g_oodle.decoder_mem) { free(g_oodle.decoder_mem); g_oodle.decoder_mem = NULL; }
    if (g_oodle.scratch_mem) { free(g_oodle.scratch_mem); g_oodle.scratch_mem = NULL; }
    memset(&g_oodle, 0, sizeof g_oodle);
}

/* 从内存中的 DLL 字节加载: 先写临时文件再 LoadLibrary */
bool poe_oodle_load_from_memory(const void *data, size_t len) {
#ifdef _WIN32
    char tmp[MAX_PATH], tmpf[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) { poe_set_error("GetTempPath 失败"); return false; }
    snprintf(tmpf, sizeof tmpf, "%soo2core_poe_%lu.dll", tmp, (unsigned long)GetCurrentProcessId());
    FILE *f = fopen(tmpf, "wb");
    if (!f) { poe_set_error("无法写入临时文件: %s", tmpf); return false; }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len) { poe_set_error("临时文件写入不完整"); remove(tmpf); return false; }
    bool ok = poe_oodle_init(tmpf);
    /* 加载后可以删除文件名 (Windows 会等到进程结束才真正删除) */
    if (ok) DeleteFileA(tmpf);
    return ok;
#else
    poe_set_error("内嵌 DLL 仅支持 Windows");
    (void)data; (void)len;
    return false;
#endif
}

static uint8_t *oodle_decompress(const uint8_t *src, size_t srclen, size_t outlen) {
    if (!g_oodle.ready) { poe_set_error("Oodle 未初始化"); return NULL; }
    uint8_t *out = (uint8_t*)malloc(outlen ? outlen : 1);
    if (!out) { poe_set_error("内存不足 (解压 %zu)", outlen); return NULL; }
    long long r = ((fn_dec)g_oodle.p_decompress)(
        src, (long long)srclen, out, (long long)outlen,
        1, 0, 0, NULL, 0, NULL, NULL,
        g_oodle.decoder_mem, (long long)g_oodle.decoder_len, 3);
    if (r != (long long)outlen) {
        free(out);
        poe_set_error("Oodle 解压失败 (%lld / %zu)", r, outlen);
        return NULL;
    }
    return out;
}

static uint8_t *oodle_compress(const uint8_t *src, size_t srclen,
                               int compressor, size_t *outlen) {
    if (!g_oodle.ready) { poe_set_error("Oodle 未初始化"); return NULL; }
    long long need = ((fn_scratch)g_oodle.p_scratch_bound)(
        compressor, 4, (long long)POE_CHUNK_SIZE, NULL);
    if (need < 0) need = 6 * 1024 * 1024;
    if (!g_oodle.scratch_mem || g_oodle.scratch_len < (size_t)need) {
        void *p = realloc(g_oodle.scratch_mem, (size_t)need);
        if (!p) { poe_set_error("内存不足 (scratch)"); return NULL; }
        g_oodle.scratch_mem = p;
        g_oodle.scratch_len = (size_t)need;
    }
    long long cap = ((fn_cbuf)g_oodle.p_cbuf_needed)(compressor, (long long)srclen);
    if (cap <= 0) { poe_set_error("GetCompressedBufferSizeNeeded 失败"); return NULL; }
    uint8_t *out = (uint8_t*)malloc((size_t)cap);
    if (!out) { poe_set_error("内存不足 (压缩缓冲)"); return NULL; }
    long long r = ((fn_comp)g_oodle.p_compress)(
        compressor, src, (long long)srclen, out, 4,
        NULL, NULL, NULL, g_oodle.scratch_mem, (long long)g_oodle.scratch_len);
    if (r <= 0) {
        free(out);
        poe_set_error("Oodle 压缩失败 (compressor=%d)", compressor);
        return NULL;
    }
    *outlen = (size_t)r;
    return out;
}

/* ============================================================ 工具 */

static size_t read_file(const char *path, uint8_t **out) {
    FILE *f = fopen(path, "rb");
    if (!f) { poe_set_error("无法打开: %s", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); poe_set_error("文件为空: %s", path); return 0; }
    uint8_t *buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { fclose(f); poe_set_error("内存不足 (%s)", path); return 0; }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) { free(buf); poe_set_error("读取不完整: %s", path); return 0; }
    *out = buf;
    return (size_t)n;
}

static int32_t rd32(const uint8_t *p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t lo = (uint64_t)(uint32_t)rd32(p);
    uint64_t hi = (uint64_t)(uint32_t)rd32(p + 4);
    return lo | (hi << 32);
}
static void wr32(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* ============================================================ Bundle 头 */

/* bundle_hdr 定义在 poe_core.h */

static void parse_bundle_hdr(const uint8_t *h, bundle_hdr *o) {
    o->uncompressed_size = rd32(h + 0);
    o->compressed_size   = rd32(h + 4);
    o->head_size         = rd32(h + 8);
    o->compressor        = rd32(h + 12);
    o->chunk_count       = rd32(h + 36);
    o->chunk_size        = rd32(h + 40);
}

/* 读取并解压整个 bundle 文件 */
uint8_t *bundle_read_all(const char *path, bundle_hdr *out_hdr, size_t *out_len) {
    uint8_t *raw = NULL;
    size_t rawlen = read_file(path, &raw);
    if (!raw) return NULL;
    if (rawlen < 60) { free(raw); poe_set_error("bundle 头不完整: %s", path); return NULL; }

    bundle_hdr h;
    parse_bundle_hdr(raw, &h);
    if (h.chunk_count < 0 || h.uncompressed_size < 0) {
        free(raw); poe_set_error("bundle 头异常: %s", path); return NULL;
    }
    size_t need = 60 + (size_t)h.chunk_count * 4;
    if (rawlen < need) { free(raw); poe_set_error("bundle 分块表不完整: %s", path); return NULL; }

    uint8_t *out = (uint8_t*)malloc((size_t)h.uncompressed_size);
    if (!out) { free(raw); poe_set_error("内存不足 (bundle %d 字节)", h.uncompressed_size); return NULL; }

    size_t pos = 60 + (size_t)h.chunk_count * 4;
    size_t outpos = 0;
    for (int32_t i = 0; i < h.chunk_count; i++) {
        int32_t cs = rd32(raw + 60 + (size_t)i * 4);
        if (cs < 0 || pos + (size_t)cs > rawlen) {
            free(raw); free(out);
            poe_set_error("分块 %d 越界: %s", i, path);
            return NULL;
        }
        size_t want = (i == h.chunk_count - 1)
                    ? (size_t)(h.uncompressed_size - h.chunk_size * i)
                    : (size_t)h.chunk_size;
        uint8_t *d = oodle_decompress(raw + pos, (size_t)cs, want);
        if (!d) { free(raw); free(out); return NULL; }
        memcpy(out + outpos, d, want);
        free(d);
        outpos += want;
        pos += (size_t)cs;
    }
    free(raw);
    if (out_hdr) *out_hdr = h;
    *out_len = (size_t)h.uncompressed_size;
    return out;
}

/* 按 offset/len 读取 (只解压涉及的块) */
static uint8_t *bundle_read_range(const char *path, int32_t off, int32_t len, size_t *out_len) {
    if (len <= 0) { *out_len = 0; return (uint8_t*)calloc(1,1); }
    uint8_t *raw = NULL;
    size_t rawlen = read_file(path, &raw);
    if (!raw) return NULL;

    bundle_hdr h;
    parse_bundle_hdr(raw, &h);
    if (h.chunk_count <= 0 || h.chunk_size <= 0) {
        free(raw); poe_set_error("bundle 参数异常"); return NULL;
    }
    int32_t base = off / h.chunk_size;
    int32_t end  = (off + len - 1) / h.chunk_size + 1;
    if (end > h.chunk_count) { free(raw); poe_set_error("读取范围超出 bundle"); return NULL; }

    size_t pos = 60 + (size_t)h.chunk_count * 4;
    for (int32_t i = 0; i < base; i++) pos += (size_t)rd32(raw + 60 + (size_t)i * 4);

    size_t total = 0;
    for (int32_t i = base; i < end; i++) {
        size_t want = (i == h.chunk_count - 1)
                    ? (size_t)(h.uncompressed_size - h.chunk_size * i)
                    : (size_t)h.chunk_size;
        total += want;
    }
    uint8_t *joined = (uint8_t*)malloc(total);
    if (!joined) { free(raw); poe_set_error("内存不足 (range)"); return NULL; }

    size_t jpos = 0;
    for (int32_t i = base; i < end; i++) {
        int32_t cs = rd32(raw + 60 + (size_t)i * 4);
        if (cs < 0 || pos + (size_t)cs > rawlen) {
            free(raw); free(joined); poe_set_error("分块越界"); return NULL;
        }
        size_t want = (i == h.chunk_count - 1)
                    ? (size_t)(h.uncompressed_size - h.chunk_size * i)
                    : (size_t)h.chunk_size;
        uint8_t *d = oodle_decompress(raw + pos, (size_t)cs, want);
        if (!d) { free(raw); free(joined); return NULL; }
        memcpy(joined + jpos, d, want);
        jpos += want;
        pos += (size_t)cs;
    }
    free(raw);

    size_t rel = (size_t)(off - base * h.chunk_size);
    uint8_t *out = (uint8_t*)malloc((size_t)len);
    if (!out) { free(joined); poe_set_error("内存不足"); return NULL; }
    memcpy(out, joined + rel, (size_t)len);
    free(joined);
    *out_len = (size_t)len;
    return out;
}

/* 生成完整 bundle 字节 */
static uint8_t *build_bundle(const uint8_t *content, size_t len,
                             int32_t compressor, size_t *out_len) {
    size_t count = len / POE_CHUNK_SIZE + (len % POE_CHUNK_SIZE ? 1 : 0);
    int32_t head_size = (int32_t)(count * 4 + 48);
    size_t cap = 60 + count * 4 + len + count * 64;
    uint8_t *out = (uint8_t*)malloc(cap);
    if (!out) { poe_set_error("内存不足 (build)"); return NULL; }
    memset(out, 0, 60);

    size_t pos = 60 + count * 4;
    size_t csize = 0;
    for (size_t i = 0; i < count; i++) {
        size_t s = i * POE_CHUNK_SIZE;
        size_t e = s + POE_CHUNK_SIZE;
        if (e > len) e = len;
        size_t cn = 0;
        uint8_t *c = oodle_compress(content + s, e - s, compressor, &cn);
        if (!c) { free(out); return NULL; }
        if (pos + cn > cap) {
            cap = (pos + cn) * 2;
            uint8_t *n = (uint8_t*)realloc(out, cap);
            if (!n) { free(c); free(out); poe_set_error("内存不足"); return NULL; }
            out = n;
        }
        memcpy(out + pos, c, cn);
        free(c);
        wr32(out + 60 + i * 4, (int32_t)cn);
        csize += cn;
        pos += cn;
    }

    wr32(out + 0,  (int32_t)len);
    wr32(out + 4,  (int32_t)csize);
    wr32(out + 8,  head_size);
    wr32(out + 12, compressor);
    wr32(out + 16, 1);
    /* 64-bit 字段 */
    out[20] = (uint8_t)(len & 0xFF); out[21] = (uint8_t)((len >> 8) & 0xFF);
    out[22] = (uint8_t)((len >> 16) & 0xFF); out[23] = (uint8_t)((len >> 24) & 0xFF);
    out[24] = out[25] = out[26] = out[27] = 0;
    out[28] = (uint8_t)(csize & 0xFF); out[29] = (uint8_t)((csize >> 8) & 0xFF);
    out[30] = (uint8_t)((csize >> 16) & 0xFF); out[31] = (uint8_t)((csize >> 24) & 0xFF);
    out[32] = out[33] = out[34] = out[35] = 0;
    wr32(out + 36, (int32_t)count);
    wr32(out + 40, POE_CHUNK_SIZE);
    *out_len = pos;
    return out;
}

/* ============================================================ 哈希 */

#define MURMUR_MARKER 0xF42A94E69CFF42FEULL
#define FNV_MARKER    0x07E47507B4A92E53ULL

static uint64_t murmur64a(const uint8_t *data, size_t len) {
    if (len == 0) return MURMUR_MARKER;
    if (data[len-1] == '/') len--;
    const uint64_t m = 0xC6A4A7935BD1E995ULL;
    const int r = 47;
    uint64_t h = (0x1337B33FULL ^ (len * m));
    size_t n = len / 8;
    for (size_t i = 0; i < n; i++) {
        uint64_t k;
        memcpy(&k, data + i * 8, 8);
        k *= m;
        h ^= (k ^ (k >> r)) * m;
        h *= m;
    }
    size_t rem = len % 8;
    if (rem) {
        uint64_t tail = 0;
        memcpy(&tail, data + n * 8, rem);
        h ^= tail;
        h *= m;
    }
    h ^= h >> r;
    h *= m;
    h ^= h >> r;
    return h;
}

static uint64_t fnv1a64(const uint8_t *data, size_t len) {
    const uint64_t prime = 0x100000001B3ULL;
    uint64_t h = 0xCBF29CE484222325ULL;
    if (len && data[len-1] == '/') {
        for (size_t i = 0; i < len - 1; i++) h = (h ^ data[i]) * prime;
    } else {
        for (size_t i = 0; i < len; i++) {
            uint8_t c = data[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            h = (h ^ c) * prime;
        }
    }
    h = (h ^ '+') * prime;
    return (h ^ '+') * prime;
}

/* 小写化并用指定算法哈希 */
static uint64_t path_hash(bool is_murmur, const char *path) {
    size_t n = strlen(path);
    uint8_t *tmp = (uint8_t*)malloc(n + 1);
    if (!tmp) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = path[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        tmp[i] = (uint8_t)c;
    }
    tmp[n] = 0;
    uint64_t h = is_murmur ? murmur64a(tmp, n) : fnv1a64(tmp, n);
    free(tmp);
    return h;
}

/* ============================================================ 索引 */

static bool grow(void **arr, int *cap, int need, size_t elemsize) {
    if (need <= *cap) return true;
    int nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    void *p = realloc(*arr, (size_t)nc * elemsize);
    if (!p) { poe_set_error("内存不足 (索引扩容)"); return false; }
    *arr = p;
    *cap = nc;
    return true;
}

bool poe_index_open(poe_index *idx, const char *index_path) {
    memset(idx, 0, sizeof *idx);
    strncpy(idx->index_path, index_path, sizeof idx->index_path - 1);

    /* 推导目录部分 */
    strncpy(idx->dir, index_path, sizeof idx->dir - 1);
    char *slash = strrchr(idx->dir, '\\');
    char *slash2 = strrchr(idx->dir, '/');
    if (slash2 > slash) slash = slash2;
    if (slash) *slash = 0;
    else idx->dir[0] = 0;

    size_t len = 0;
    bundle_hdr h;
    uint8_t *d = NULL;

    /* ---------- 判断容器类型 ---------- */
    if (poe_is_ggpk_file(index_path)) {
        /* 独立客户端: 从 GGPK 中取 Bundles2/_.index.bin */
        poe_ggpk *g = (poe_ggpk*)calloc(1, sizeof(poe_ggpk));
        if (!g) { poe_set_error("内存不足"); return false; }
        if (!poe_ggpk_open(g, index_path)) { free(g); return false; }
        idx->ggpk = g;
        idx->is_ggpk = true;

        uint8_t *raw = poe_ggpk_read(g, "Bundles2/_.index.bin", &len);
        if (!raw) {
            poe_ggpk_close(g);
            free(g);
            idx->ggpk = NULL;
            poe_set_error("GGPK 中找不到 Bundles2/_.index.bin (可能不是完整安装)");
            return false;
        }
        /* 从内存解析 bundle (临时落盘以复用现有逻辑) */
        char tmp[1400];
#ifdef _WIN32
        /* 优先放在 GGPK 同目录, 失败则退回系统临时目录 */
        snprintf(tmp, sizeof tmp, "__poe_idx_tmp.bin");
        FILE *probe = fopen(tmp, "wb");
        if (!probe) {
            char td[MAX_PATH];
            DWORD n = GetTempPathA(MAX_PATH, td);
            if (n > 0 && n < MAX_PATH)
                snprintf(tmp, sizeof tmp, "%spoe_idx_tmp_%lu.bin", td,
                         (unsigned long)GetCurrentProcessId());
        } else {
            fclose(probe);
        }
#else
        snprintf(tmp, sizeof tmp, "%s/__poe_idx_tmp.bin", idx->dir[0] ? idx->dir : ".");
#endif
        FILE *tf = fopen(tmp, "wb");
        if (!tf || fwrite(raw, 1, len, tf) != len) {
            if (tf) fclose(tf);
            free(raw);
            poe_ggpk_close(g); free(g); idx->ggpk = NULL;
            poe_set_error("无法创建临时索引文件 (%s)", tmp);
            remove(tmp);
            return false;
        }
        fclose(tf);
        d = bundle_read_all(tmp, &h, &len);
        remove(tmp);
        free(raw);
    } else {
        /* Steam 版: 直接读 _.index.bin */
        d = bundle_read_all(index_path, &h, &len);
    }

    if (!d) return false;
    idx->compressor = h.compressor;
    idx->chunk_size = h.chunk_size;

    size_t p = 0;
    if (len < 4) { free(d); poe_set_error("索引数据过短"); return false; }

    int32_t nb = rd32(d + p); p += 4;
    if (nb < 0 || nb > POE_MAX_BUNDLES) {
        free(d); poe_set_error("bundle 数量异常: %d", nb); return false;
    }
    idx->bundles = (poe_bundle_rec*)calloc((size_t)nb, sizeof(poe_bundle_rec));
    if (!idx->bundles) { free(d); poe_set_error("内存不足"); return false; }
    idx->cbundles = nb;
    idx->nbundles = nb;

    for (int i = 0; i < nb; i++) {
        if (p + 4 > len) { free(d); poe_set_error("索引损坏 (bundle 路径长度)"); return false; }
        int32_t pl = rd32(d + p); p += 4;
        if (pl < 0 || p + (size_t)pl + 4 > len) {
            free(d); poe_set_error("索引损坏 (bundle 路径)"); return false;
        }
        idx->bundles[i].path = (char*)malloc((size_t)pl + 1);
        if (!idx->bundles[i].path) { free(d); poe_set_error("内存不足"); return false; }
        memcpy(idx->bundles[i].path, d + p, (size_t)pl);
        idx->bundles[i].path[pl] = 0;
        p += (size_t)pl;
        idx->bundles[i].uncompressed = rd32(d + p); p += 4;
    }

    if (p + 4 > len) { free(d); poe_set_error("索引损坏 (文件数)"); return false; }
    int32_t nf = rd32(d + p); p += 4;
    if (nf < 0 || nf > POE_MAX_FILES) {
        free(d); poe_set_error("文件数量异常: %d", nf); return false;
    }
    idx->files = (poe_file_rec*)malloc((size_t)nf * sizeof(poe_file_rec));
    if (!idx->files) { free(d); poe_set_error("内存不足 (文件表)"); return false; }
    idx->nfiles = nf; idx->cfiles = nf;
    for (int i = 0; i < nf; i++) {
        if (p + 20 > len) { free(d); poe_set_error("索引损坏 (文件记录)"); return false; }
        idx->files[i].hash   = rd64(d + p);
        idx->files[i].bundle = rd32(d + p + 8);
        idx->files[i].offset = rd32(d + p + 12);
        idx->files[i].size   = rd32(d + p + 16);
        p += 20;
    }

    if (p + 4 > len) { free(d); poe_set_error("索引损坏 (目录数)"); return false; }
    int32_t nd = rd32(d + p); p += 4;
    if (nd < 0) { free(d); poe_set_error("目录数量异常"); return false; }
    idx->dirs = (poe_dir_rec*)malloc((size_t)(nd ? nd : 1) * sizeof(poe_dir_rec));
    if (!idx->dirs) { free(d); poe_set_error("内存不足 (目录表)"); return false; }
    idx->ndirs = nd; idx->cdirs = nd;
    for (int i = 0; i < nd; i++) {
        if (p + 20 > len) { free(d); poe_set_error("索引损坏 (目录记录)"); return false; }
        idx->dirs[i].hash    = rd64(d + p);
        idx->dirs[i].offset  = rd32(d + p + 8);
        idx->dirs[i].size    = rd32(d + p + 12);
        idx->dirs[i].recsize = rd32(d + p + 16);
        p += 20;
    }

    idx->dir_bundle_len = len - p;
    idx->dir_bundle_data = (uint8_t*)malloc(idx->dir_bundle_len ? idx->dir_bundle_len : 1);
    if (!idx->dir_bundle_data) { free(d); poe_set_error("内存不足 (目录数据)"); return false; }
    if (idx->dir_bundle_len) memcpy(idx->dir_bundle_data, d + p, idx->dir_bundle_len);

    idx->is_murmur = (nd > 0 && idx->dirs[0].hash == MURMUR_MARKER);
    if (nd > 0 && !idx->is_murmur && idx->dirs[0].hash != FNV_MARKER) {
        poe_set_error("无法识别哈希算法: 0x%016llX", (unsigned long long)idx->dirs[0].hash);
        free(d);
        return false;
    }
    free(d);
    return true;
}

void poe_index_close(poe_index *idx) {
    if (idx->bundles) {
        for (int i = 0; i < idx->nbundles; i++) free(idx->bundles[i].path);
        free(idx->bundles);
    }
    free(idx->files);
    free(idx->dirs);
    free(idx->dir_bundle_data);
    if (idx->ggpk) {
        poe_ggpk *g = (poe_ggpk*)idx->ggpk;
        poe_ggpk_close(g);
        free(g);
    }
    memset(idx, 0, sizeof *idx);
}

static poe_file_rec *find_file(poe_index *idx, const char *path) {
    uint64_t h = path_hash(idx->is_murmur, path);
    for (int i = 0; i < idx->nfiles; i++)
        if (idx->files[i].hash == h) return &idx->files[i];
    return NULL;
}

bool poe_index_has(poe_index *idx, const char *path) {
    return find_file(idx, path) != NULL;
}

/* 把整个 bundle 读为内存 (Steam: 磁盘文件; GGPK: 从包内取) */
static uint8_t *bundle_fetch_all(poe_index *idx, int bundle_index,
                                 bundle_hdr *out_hdr, size_t *out_len) {
    if (bundle_index < 0 || bundle_index >= idx->nbundles) {
        poe_set_error("bundle 索引越界");
        return NULL;
    }
    const char *rel = idx->bundles[bundle_index].path;

    if (idx->is_ggpk) {
        poe_ggpk *g = (poe_ggpk*)idx->ggpk;
        char vpath[1400];
        snprintf(vpath, sizeof vpath, "Bundles2/%s.bundle.bin", rel);
        uint8_t *raw = poe_ggpk_read(g, vpath, out_len);
        if (!raw) return NULL;
        /* 走与磁盘相同的解析流程: 需要文件句柄, 因此用临时文件 */
        char tmp[1400];
        snprintf(tmp, sizeof tmp, "__poe_bundle_tmp.bin");
        FILE *tf = fopen(tmp, "wb");
        if (!tf || fwrite(raw, 1, *out_len, tf) != *out_len) {
            if (tf) fclose(tf);
            free(raw);
            poe_set_error("无法创建临时 bundle 文件");
            return NULL;
        }
        fclose(tf);
        size_t sz = *out_len;
        uint8_t *d = bundle_read_all(tmp, out_hdr, out_len);
        remove(tmp);
        free(raw);
        (void)sz;
        return d;
    }

    char bp[1400];
    snprintf(bp, sizeof bp, "%s\\%s.bundle.bin", idx->dir, rel);
#ifdef _WIN32
    for (char *q = bp; *q; q++) if (*q == '/') *q = '\\';
#endif
    return bundle_read_all(bp, out_hdr, out_len);
}

uint8_t *poe_index_read(poe_index *idx, const char *path, size_t *out_len) {
    poe_file_rec *r = find_file(idx, path);
    if (!r) { poe_set_error("索引中找不到: %s", path); return NULL; }
    if (r->bundle < 0 || r->bundle >= idx->nbundles) {
        poe_set_error("bundle 索引越界");
        return NULL;
    }

    if (idx->is_ggpk) {
        /* GGPK: bundle 在 GGPK 里的名字就是 lib 路径本身 (不带 .bundle.bin) */
        poe_ggpk *g = (poe_ggpk*)idx->ggpk;
        char vpath[1400];
        snprintf(vpath, sizeof vpath, "Bundles2/%s.bundle.bin", idx->bundles[r->bundle].path);
        size_t blen = 0;
        uint8_t *raw = poe_ggpk_read(g, vpath, &blen);
        if (!raw) return NULL;

        char tmp[1400];
        snprintf(tmp, sizeof tmp, "__poe_rng_tmp.bin");
        FILE *tf = fopen(tmp, "wb");
        if (!tf || fwrite(raw, 1, blen, tf) != blen) {
            if (tf) fclose(tf);
            free(raw);
            poe_set_error("无法创建临时文件");
            return NULL;
        }
        fclose(tf);
        free(raw);

        uint8_t *out = bundle_read_range(tmp, r->offset, r->size, out_len);
        remove(tmp);
        return out;
    }

    char bp[1400];
    snprintf(bp, sizeof bp, "%s\\%s.bundle.bin", idx->dir, idx->bundles[r->bundle].path);
#ifdef _WIN32
    for (char *q = bp; *q; q++) if (*q == '/') *q = '\\';
#endif
    return bundle_read_range(bp, r->offset, r->size, out_len);
}

/* ---------- 目标路径解析 ---------- */

bool poe_resolve_target(const char *input, char *out, size_t out_cap, bool *is_ggpk) {
    if (is_ggpk) *is_ggpk = false;
    if (!input || !input[0]) return false;

    FILE *f = fopen(input, "rb");
    if (f) {
        fclose(f);
        if (poe_is_ggpk_file(input)) {
            strncpy(out, input, out_cap - 1); out[out_cap-1] = 0;
            if (is_ggpk) *is_ggpk = true;
            return true;
        }
        /* 可能是 _.index.bin */
        const char *base = input;
        for (const char *p = input; *p; p++) if (*p=='\\'||*p=='/') base = p+1;
        if (_stricmp(base, "_.index.bin") == 0) {
            strncpy(out, input, out_cap - 1); out[out_cap-1] = 0;
            return true;
        }
        /* 其他 .bin 也接受 */
        size_t L = strlen(base);
        if (L > 4 && _stricmp(base + L - 4, ".bin") == 0) {
            strncpy(out, input, out_cap - 1); out[out_cap-1] = 0;
            return true;
        }
    }
    return false;
}


/* ============================================================ Languages.dat */

/* UTF-16LE (以 0x0000 结尾) -> UTF-8 */
static int u16_to_u8(const uint8_t *src, size_t maxbytes, char *dst, size_t cap, size_t *consumed) {
    size_t i = 0, o = 0;
    while (i + 1 < maxbytes) {
        uint32_t c = (uint32_t)src[i] | ((uint32_t)src[i+1] << 8);
        if (c == 0) { if (consumed) *consumed = i + 2; break; }
        if (c < 0x80) {
            if (o + 1 >= cap) break;
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= cap) break;
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= cap) break;
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
        i += 2;
    }
    if (o < cap) dst[o] = 0;
    return (int)o;
}

/* 找 UTF-16LE 字符串起点, 返回 0x0000 之后的位置 */
static size_t u16_scan(const uint8_t *d, size_t len, size_t p) {
    while (p + 1 < len) {
        if (d[p] == 0 && d[p+1] == 0) return p + 2;
        p += 2;
    }
    return len;
}

bool poe_lang_parse(const uint8_t *data, size_t len,
                    poe_lang_row *rows, int max_rows, int *out_count) {
    if (len < 4) { poe_set_error("Languages.dat 过短"); return false; }
    int32_t n = rd32(data);
    if (n <= 0 || n > max_rows) { poe_set_error("行数异常: %d", n); return false; }

    size_t p = 4;
    for (int i = 0; i < n; i++) {
        if (p + 4 > len) { poe_set_error("结构损坏 (行号)"); return false; }
        p += 4; /* row_index */
        size_t c1;
        u16_to_u8(data + p, len - p, rows[i].name, sizeof rows[i].name, &c1);
        p += c1;
        if (p > len) { poe_set_error("结构损坏 (name)"); return false; }
        u16_to_u8(data + p, len - p, rows[i].native_name, sizeof rows[i].native_name, &c1);
        p += c1;
        if (p > len) { poe_set_error("结构损坏 (native)"); return false; }
        u16_to_u8(data + p, len - p, rows[i].code, sizeof rows[i].code, &c1);
        p += c1;

        /* 跳过标量区: 找下一个行号 (i+1), 且其后必须是合法的 UTF-16 字符串 */
        if (i + 1 < n) {
            size_t q = p;
            bool found = false;
            while (q + 4 <= len) {
                if (rd32(data + q) == i + 1) {
                    /* 校验后接字符串: 首 2 字节是 ASCII 字母, 且能走到 0x0000 */
                    size_t t = q + 4;
                    if (t + 4 <= len) {
                        uint32_t c0 = (uint32_t)data[t] | ((uint32_t)data[t+1] << 8);
                        bool ascii_alpha = (c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z');
                        if (ascii_alpha) {
                            /* 必须在合理距离内遇到结尾 0x0000 */
                            size_t lim = t + 2 * 200;
                            if (lim > len) lim = len;
                            bool term = false;
                            for (size_t k = t; k + 1 < lim; k += 2) {
                                if (data[k] == 0 && data[k+1] == 0) { term = true; break; }
                            }
                            if (term) { found = true; break; }
                        }
                    }
                }
                q++;
            }
            if (!found) { poe_set_error("无法定位第 %d 行", i + 1); return false; }
            p = q;
        } else {
            p = len;
        }
    }
    *out_count = n;
    return true;
}

bool poe_lang_is_applied(const poe_lang_row *rows, int count) {
    for (int i = 0; i < count; i++)
        if (strcmp(rows[i].code, "fr") == 0)
            return strcmp(rows[i].name, "Traditional Chinese") == 0;
    return false;
}

typedef struct { size_t name_off, name_len, nat_off, nat_len, code_off, code_len; } row_span;

uint8_t *poe_lang_apply(const uint8_t *data, size_t len, size_t *out_len) {
    poe_lang_row rows[64];
    int count = 0;
    if (!poe_lang_parse(data, len, rows, 64, &count)) return NULL;

    int ifr = -1, itc = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(rows[i].code, "fr") == 0) ifr = i;
        if (strcmp(rows[i].code, "zh-TW") == 0) itc = i;
    }
    if (ifr < 0 || itc < 0) {
        poe_set_error("找不到 fr 或 zh-TW 语言行");
        return NULL;
    }

    /* 先扫描一次, 记录每行的 3 个字符串区间与标量区间 */
    typedef struct { size_t n0,n1,n2,c0,c1,c2,scal_s,scal_e; } span_t;
    span_t sp[64];
    size_t p = 4;
    for (int i = 0; i < count; i++) {
        if (p + 4 > len) { poe_set_error("结构损坏"); return NULL; }
        p += 4;                       /* row_index */
        sp[i].n0 = p; p = u16_scan(data, len, p);
        sp[i].n1 = p; p = u16_scan(data, len, p);
        sp[i].n2 = p; p = u16_scan(data, len, p);
        sp[i].scal_s = p;
        if (i + 1 < count) {
            size_t q = p;
            bool found = false;
            while (q + 4 <= len) {
                if (rd32(data + q) == i + 1) {
                    size_t t = q + 4;
                    if (t + 4 <= len) {
                        uint32_t c0 = (uint32_t)data[t] | ((uint32_t)data[t+1] << 8);
                        if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) { found = true; break; }
                    }
                }
                q++;
            }
            if (!found) { poe_set_error("无法定位第 %d 行", i + 1); return NULL; }
            sp[i].scal_e = q;
            p = q;
        } else {
            sp[i].scal_e = len;
            p = len;
        }
    }

    /* 计算新总长度 */
    size_t cap = len + 512;
    uint8_t *out = (uint8_t*)malloc(cap);
    if (!out) { poe_set_error("内存不足"); return NULL; }
    size_t o = 0;
    memcpy(out, data, 4); o = 4;

    for (int i = 0; i < count; i++) {
        /* row_index 原样 */
        memcpy(out + o, data + sp[i].n0 - 4, 4); o += 4;

        const char *nm, *nt, *cd;
        if (i == ifr) {
            nm = rows[itc].name; nt = rows[itc].native_name; cd = rows[ifr].code;
        } else if (i == itc) {
            nm = rows[ifr].name; nt = rows[ifr].native_name; cd = rows[itc].code;
        } else {
            nm = rows[i].name; nt = rows[i].native_name; cd = rows[i].code;
        }

        const char *parts[3] = { nm, nt, cd };
        for (int k = 0; k < 3; k++) {
            const char *s = parts[k];
            while (*s) {
                unsigned char ch = (unsigned char)*s;
                uint32_t cp;
                if (ch < 0x80) { cp = ch; s += 1; }
                else if ((ch & 0xE0) == 0xC0 && s[1]) {
                    cp = ((uint32_t)(ch & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F); s += 2;
                } else if ((ch & 0xF0) == 0xE0 && s[1] && s[2]) {
                    cp = ((uint32_t)(ch & 0x0F) << 12) |
                         ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
                    s += 3;
                } else { cp = '?'; s += 1; }
                if (o + 2 > cap) { cap *= 2; out = (uint8_t*)realloc(out, cap); }
                out[o++] = (uint8_t)(cp & 0xFF);
                out[o++] = (uint8_t)((cp >> 8) & 0xFF);
            }
            out[o++] = 0; out[o++] = 0;
        }

        /* 标量区原样复制 */
        size_t sl = sp[i].scal_e - sp[i].scal_s;
        if (o + sl > cap) { cap = (o + sl) * 2; out = (uint8_t*)realloc(out, cap); }
        if (sl) { memcpy(out + o, data + sp[i].scal_s, sl); o += sl; }
    }
    *out_len = o;
    return out;
}
/* ============================================================ UISettings XML */

/* 检测编码并提取 typeface */
bool poe_xml_get_typeface(const uint8_t *data, size_t len,
                          char *face, size_t face_cap, bool *all_same) {
    if (len < 2) return false;
    bool le = (data[0] == 0xFF && data[1] == 0xFE);
    bool be = (data[0] == 0xFE && data[1] == 0xFF);
    size_t start = (le || be) ? 2 : 0;

    char first[128] = {0};
    bool found = false, same = true;

    /* 在原始字节里找 typeface="..." */
    const char *key = "typeface=\"";
    size_t klen = strlen(key);
    for (size_t i = start; i + klen < len; i++) {
        bool match = true;
        if (le) {
            for (size_t k = 0; k < klen; k++) {
                if (!(data[i + k*2] == (uint8_t)key[k] && data[i + k*2 + 1] == 0)) { match = false; break; }
            }
            if (!match) continue;
            char tmp[128]; size_t ti = 0;
            size_t q = i + klen * 2;
            while (q + 1 < len && !(data[q] == '"' && data[q+1] == 0) && ti < sizeof tmp - 1) {
                uint32_t c = (uint32_t)data[q] | ((uint32_t)data[q+1] << 8);
                tmp[ti++] = (c < 0x80) ? (char)c : '?';
                q += 2;
            }
            tmp[ti] = 0;
            if (!found) { strncpy(first, tmp, sizeof first - 1); found = true; }
            else if (strcmp(first, tmp) != 0) same = false;
        } else if (be) {
            for (size_t k = 0; k < klen; k++) {
                if (!(data[i + k*2 + 1] == (uint8_t)key[k] && data[i + k*2] == 0)) { match = false; break; }
            }
            if (!match) continue;
            char tmp[128]; size_t ti = 0;
            size_t q = i + klen * 2;
            while (q + 1 < len && !(data[q+1] == '"' && data[q] == 0) && ti < sizeof tmp - 1) {
                uint32_t c = ((uint32_t)data[q] << 8) | (uint32_t)data[q+1];
                tmp[ti++] = (c < 0x80) ? (char)c : '?';
                q += 2;
            }
            tmp[ti] = 0;
            if (!found) { strncpy(first, tmp, sizeof first - 1); found = true; }
            else if (strcmp(first, tmp) != 0) same = false;
        } else {
            if (memcmp(data + i, key, klen) != 0) continue;
            char tmp[128]; size_t ti = 0;
            size_t q = i + klen;
            while (q < len && data[q] != '"' && ti < sizeof tmp - 1) tmp[ti++] = (char)data[q++];
            tmp[ti] = 0;
            if (!found) { strncpy(first, tmp, sizeof first - 1); found = true; }
            else if (strcmp(first, tmp) != 0) same = false;
        }
    }
    if (!found) return false;
    if (face) { strncpy(face, first, face_cap - 1); face[face_cap-1] = 0; }
    if (all_same) *all_same = same;
    return true;
}

uint8_t *poe_xml_set_typeface(const uint8_t *data, size_t len,
                              const char *face, size_t *out_len) {
    if (len < 2) { poe_set_error("XML 过短"); return NULL; }
    bool le = (data[0] == 0xFF && data[1] == 0xFE);
    bool be = (data[0] == 0xFE && data[1] == 0xFF);

    size_t cap = len * 3 + 4096;
    uint8_t *out = (uint8_t*)malloc(cap);
    if (!out) { poe_set_error("内存不足 (XML)"); return NULL; }

    const char *key = "typeface=\"";
    size_t klen = strlen(key);
    size_t o = 0, i = (le || be) ? 2 : 0;
    if (le || be) { out[o++] = data[0]; out[o++] = data[1]; }

    while (i < len) {
        bool hit = false;
        if (le) {
            if (i + klen*2 <= len) {
                hit = true;
                for (size_t k = 0; k < klen; k++)
                    if (!(data[i+k*2] == (uint8_t)key[k] && data[i+k*2+1] == 0)) { hit = false; break; }
            }
        } else if (be) {
            if (i + klen*2 <= len) {
                hit = true;
                for (size_t k = 0; k < klen; k++)
                    if (!(data[i+k*2+1] == (uint8_t)key[k] && data[i+k*2] == 0)) { hit = false; break; }
            }
        } else {
            if (i + klen <= len && memcmp(data + i, key, klen) == 0) hit = true;
        }

        if (!hit) {
            if (o + 1 >= cap) break;
            out[o++] = data[i++];
            continue;
        }

        /* 复制 key */
        size_t adv = (le || be) ? klen * 2 : klen;
        memcpy(out + o, data + i, adv); o += adv; i += adv;

        /* 跳过旧值到引号 */
        while (i < len) {
            if (le) {
                if (i + 1 < len && data[i] == '"' && data[i+1] == 0) { i += 2; break; }
                i += 2;
            } else if (be) {
                if (i + 1 < len && data[i+1] == '"' && data[i] == 0) { i += 2; break; }
                i += 2;
            } else {
                if (data[i] == '"') { i++; break; }
                i++;
            }
        }

        /* 写入新值 */
        for (const char *s = face; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (le) { out[o++] = c; out[o++] = 0; }
            else if (be) { out[o++] = 0; out[o++] = c; }
            else out[o++] = c;
        }
        if (o + 2 >= cap) { free(out); poe_set_error("缓冲不足"); return NULL; }
        if (le) { out[o++] = '"'; out[o++] = 0; }
        else if (be) { out[o++] = 0; out[o++] = '"'; }
        else out[o++] = '"';
    }
    *out_len = o;
    return out;
}

/* ============================================================ 写回 */

static uint8_t *serialize_index(poe_index *idx, size_t *out_len) {
    size_t cap = 4 + (size_t)idx->nbundles * 300 + 4 +
                 (size_t)idx->nfiles * 20 + 4 +
                 (size_t)idx->ndirs * 20 + idx->dir_bundle_len + 256;
    uint8_t *out = (uint8_t*)malloc(cap);
    if (!out) { poe_set_error("内存不足 (序列化)"); return NULL; }
    size_t o = 0;
    wr32(out + o, idx->nbundles); o += 4;
    for (int i = 0; i < idx->nbundles; i++) {
        size_t pl = strlen(idx->bundles[i].path);
        wr32(out + o, (int32_t)pl); o += 4;
        memcpy(out + o, idx->bundles[i].path, pl); o += pl;
        wr32(out + o, idx->bundles[i].uncompressed); o += 4;
    }
    wr32(out + o, idx->nfiles); o += 4;
    for (int i = 0; i < idx->nfiles; i++) {
        uint64_t h = idx->files[i].hash;
        memcpy(out + o, &h, 8); o += 8;
        wr32(out + o, idx->files[i].bundle); o += 4;
        wr32(out + o, idx->files[i].offset); o += 4;
        wr32(out + o, idx->files[i].size); o += 4;
    }
    wr32(out + o, idx->ndirs); o += 4;
    for (int i = 0; i < idx->ndirs; i++) {
        uint64_t h = idx->dirs[i].hash;
        memcpy(out + o, &h, 8); o += 8;
        wr32(out + o, idx->dirs[i].offset); o += 4;
        wr32(out + o, idx->dirs[i].size); o += 4;
        wr32(out + o, idx->dirs[i].recsize); o += 4;
    }
    if (idx->dir_bundle_len) {
        memcpy(out + o, idx->dir_bundle_data, idx->dir_bundle_len);
        o += idx->dir_bundle_len;
    }
    *out_len = o;
    return out;
}

static bool write_file_atomic(const char *path, const uint8_t *data, size_t len) {
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        poe_set_error("无法写入 %s (请关闭游戏，或以管理员身份运行)", tmp);
        return false;
    }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len) {
        poe_set_error("写入不完整: %s", tmp);
        remove(tmp);
        return false;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        if (rename(tmp, path) != 0) {
            poe_set_error("重命名失败: %s (文件可能被占用)", path);
            remove(tmp);
            return false;
        }
    }
    return true;
}

/* GGPK 模式写入: 新建 LibGGPK3 bundle, 重写索引 */
static bool write_back_ggpk(poe_index *idx, const poe_write_item *items, int n) {
    poe_ggpk *g = (poe_ggpk*)calloc(1, sizeof(poe_ggpk));
    if (!g) { poe_set_error("内存不足"); return false; }
    if (!poe_ggpk_open(g, idx->index_path)) { free(g); return false; }

    /* 1. 组装新的 LibGGPK3/0 bundle 内容 */
    const char *custom_name = "LibGGPK3/0";

    /* 若已存在, 先读取其内容 */
    uint8_t *existing = NULL;
    size_t existing_len = 0;
    {
        char vp[1400];
        snprintf(vp, sizeof vp, "Bundles2/%s.bundle.bin", custom_name);
        poe_ggpk_entry e;
        if (poe_ggpk_find(g, vp, &e)) {
            size_t rawlen = 0;
            uint8_t *raw = poe_ggpk_read(g, vp, &rawlen);
            if (raw) {
                char tp[1400];
                snprintf(tp, sizeof tp, "__ggpk_exist.bin");
                FILE *t = fopen(tp, "wb");
                if (t) {
                    fwrite(raw, 1, rawlen, t);
                    fclose(t);
                    bundle_hdr oh;
                    existing = bundle_read_all(tp, &oh, &existing_len);
                    remove(tp);
                }
                free(raw);
            }
        }
    }

    size_t total = existing_len;
    for (int i = 0; i < n; i++) total += items[i].len;

    uint8_t *blob = (uint8_t*)malloc(total ? total : 1);
    if (!blob) { free(existing); poe_ggpk_close(g); free(g); poe_set_error("内存不足"); return false; }
    if (existing_len) memcpy(blob, existing, existing_len);
    free(existing);

    /* 添加 custom bundle 到索引 */
    int custom_idx = -1;
    for (int i = 0; i < idx->nbundles; i++)
        if (strcmp(idx->bundles[i].path, custom_name) == 0) { custom_idx = i; break; }
    if (custom_idx < 0) {
        if (!grow((void**)&idx->bundles, &idx->cbundles, idx->nbundles + 1, sizeof(poe_bundle_rec))) {
            free(blob); poe_ggpk_close(g); free(g); return false;
        }
        custom_idx = idx->nbundles++;
        char *np = (char*)malloc(strlen(custom_name) + 1);
        if (!np) { free(blob); poe_ggpk_close(g); free(g); poe_set_error("内存不足"); return false; }
        strcpy(np, custom_name);
        idx->bundles[custom_idx].path = np;
        idx->bundles[custom_idx].uncompressed = 0;
    }

    size_t pos = existing_len;
    for (int i = 0; i < n; i++) {
        poe_file_rec *r = find_file(idx, items[i].path);
        if (!r) { free(blob); poe_ggpk_close(g); free(g); poe_set_error("索引中找不到 %s", items[i].path); return false; }
        memcpy(blob + pos, items[i].data, items[i].len);
        r->bundle = custom_idx;
        r->offset = (int32_t)pos;
        r->size = (int32_t)items[i].len;
        pos += items[i].len;
    }
    idx->bundles[custom_idx].uncompressed = (int32_t)total;

    /* 2. 压缩 bundle */
    size_t blen = 0;
    uint8_t *bb = build_bundle(blob, total, idx->compressor, &blen);
    free(blob);
    if (!bb) { poe_ggpk_close(g); free(g); return false; }

    /* 3. 写 bundle 到 GGPK */
    char vpath[1400];
    snprintf(vpath, sizeof vpath, "Bundles2/%s.bundle.bin", custom_name);
    bool ok = poe_ggpk_write_file(g, vpath, bb, blen);
    free(bb);

    /* 4. 重写索引 */
    if (ok) {
        size_t ilen = 0;
        uint8_t *ir = serialize_index(idx, &ilen);
        if (ir) {
            size_t icl = 0;
            uint8_t *ib = build_bundle(ir, ilen, idx->compressor, &icl);
            free(ir);
            if (ib) {
                ok = poe_ggpk_write_file(g, "Bundles2/_.index.bin", ib, icl);
                free(ib);
            } else ok = false;
        } else ok = false;
    }

    poe_ggpk_close(g);
    free(g);
    return ok;
}
bool poe_write_back(poe_index *idx, const poe_write_item *items, int n) {
    if (idx->is_ggpk)
        return write_back_ggpk(idx, items, n);

    /* 修改前自动备份 (仅首次生效, 保证回退到最早的原始状态) */
    if (!poe_make_backup(idx->dir, idx->index_path)) {
        printf("  [警告] 备份失败: %s\n", poe_last_error);
        printf("  [警告] 继续写入, 但将无法回退\n");
    } else {
        printf("  [备份] 原始索引已保存\n");
    }

    int custom = -1;
    for (int i = 0; i < idx->nbundles; i++)
        if (strncmp(idx->bundles[i].path, POE_CUSTOM_PREFIX, strlen(POE_CUSTOM_PREFIX)) == 0)
            custom = i;

    uint8_t *existing = NULL;
    size_t existing_len = 0;
    if (custom < 0) {
        if (!grow((void**)&idx->bundles, &idx->cbundles, idx->nbundles + 1, sizeof(poe_bundle_rec)))
            return false;
        custom = idx->nbundles++;
        char *np = (char*)malloc(32);
        if (!np) { poe_set_error("内存不足"); return false; }
        strcpy(np, POE_CUSTOM_PREFIX "0");
        idx->bundles[custom].path = np;
        idx->bundles[custom].uncompressed = 0;
    } else {
        char bp[1400];
        snprintf(bp, sizeof bp, "%s\\%s.bundle.bin", idx->dir, idx->bundles[custom].path);
        bundle_hdr h;
        existing = bundle_read_all(bp, &h, &existing_len);
        if (!existing) existing_len = 0;   /* 不存在则视为空 */
    }

    size_t total = existing_len;
    for (int i = 0; i < n; i++) total += items[i].len;
    uint8_t *blob = (uint8_t*)malloc(total ? total : 1);
    if (!blob) { free(existing); poe_set_error("内存不足 (合并)"); return false; }
    if (existing_len) memcpy(blob, existing, existing_len);
    free(existing);

    size_t pos = existing_len;
    for (int i = 0; i < n; i++) {
        poe_file_rec *r = find_file(idx, items[i].path);
        if (!r) { free(blob); poe_set_error("索引中找不到 %s", items[i].path); return false; }
        memcpy(blob + pos, items[i].data, items[i].len);
        r->bundle = custom;
        r->offset = (int32_t)pos;
        r->size   = (int32_t)items[i].len;
        pos += items[i].len;
    }
    idx->bundles[custom].uncompressed = (int32_t)total;

    size_t blen = 0;
    uint8_t *bb = build_bundle(blob, total, idx->compressor, &blen);
    free(blob);
    if (!bb) return false;

    size_t ilen = 0;
    uint8_t *ir = serialize_index(idx, &ilen);
    if (!ir) { free(bb); return false; }
    size_t iclen = 0;
    uint8_t *ib = build_bundle(ir, ilen, idx->compressor, &iclen);
    free(ir);
    if (!ib) { free(bb); return false; }

    /* 自校验: 写临时文件, 解压再比对 (放当前目录, 避免权限问题) */
    {
        char vp[1400];
        snprintf(vp, sizeof vp, "__verify_tmp_%lu.bin", (unsigned long)clock());
        FILE *vf = fopen(vp, "wb");
        if (!vf) {
            /* 退一步: 尝试 Bundles2 目录 */
            snprintf(vp, sizeof vp, "%s\\__verify_tmp.bin", idx->dir);
            vf = fopen(vp, "wb");
        }
        if (!vf) {
            free(bb); free(ib);
            poe_set_error("无法创建校验临时文件: %s", vp);
            return false;
        }
        fwrite(ib, 1, iclen, vf);
        fclose(vf);
        bundle_hdr vh;
        size_t vlen = 0;
        uint8_t *v = bundle_read_all(vp, &vh, &vlen);
        remove(vp);
        if (!v || vlen < 4) {
            free(v); free(bb); free(ib);
            poe_set_error("自校验失败: 索引无法解压");
            return false;
        }
        int32_t vnb = rd32(v);
        free(v);
        if (vnb != idx->nbundles) {
            free(bb); free(ib);
            poe_set_error("自校验失败: bundle 数不符 (%d != %d)", vnb, idx->nbundles);
            return false;
        }
        printf("  [自校验] 索引往返 OK (%d bundles)\n", vnb);
    }

    char cpath[1400], ipath[1400];
    snprintf(cpath, sizeof cpath, "%s\\%s.bundle.bin", idx->dir, idx->bundles[custom].path);
#ifdef _WIN32
    /* bundle 路径内部用 '/', Windows 需转成 '\\' */
    for (char *p = cpath; *p; p++) if (*p == '/') *p = '\\';
#endif
    /* 确保子目录存在 (LibGGPK3/0.bundle.bin 需要建目录) */
    {
        char dircopy[1400];
        strncpy(dircopy, cpath, sizeof dircopy - 1);
        dircopy[sizeof dircopy - 1] = 0;
        char *s = strrchr(dircopy, '\\');
        if (!s) s = strrchr(dircopy, '/');
        if (s) *s = 0;
#ifdef _WIN32
        CreateDirectoryA(dircopy, NULL);
#else
        mkdir(dircopy, 0755);
#endif
    }
    snprintf(ipath, sizeof ipath, "%s", idx->index_path);

    bool ok2 = write_file_atomic(cpath, bb, blen) && write_file_atomic(ipath, ib, iclen);
    free(bb);
    free(ib);
    return ok2;
}
/* ============================================================ 字体枚举 */

#ifdef _WIN32
/* 检查某个字体名在本机是否存在 */
static bool font_exists(const char *name) {
    HFONT f = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, name);
    if (!f) return false;
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, f);
    char got[256] = {0};
    GetTextFaceA(dc, sizeof got, got);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    DeleteObject(f);
    return got[0] && _stricmp(got, name) == 0;
}
#endif

int poe_enum_fonts(char names[][128], int max) {
    static const char *cands[] = {
        "HarmonyOS Sans TC",
        "HarmonyOS Sans SC",
        "HarmonyOS Sans",
        "Microsoft JhengHei",
        "Microsoft JhengHei UI",
        "Microsoft YaHei",
        "Microsoft YaHei UI",
        "Noto Sans CJK TC",
        "Noto Sans CJK SC",
        "Source Han Sans TC",
        "Source Han Sans SC",
        "PingFang TC",
        "MingLiU",
        "PMingLiU",
        "DFKai-SB",
        "SimSun",
        "Segoe UI",
        "Arial",
        NULL
    };
    int n = 0;
#ifdef _WIN32
    for (int i = 0; cands[i] && n < max; i++) {
        if (font_exists(cands[i])) {
            strncpy(names[n], cands[i], 127);
            names[n][127] = 0;
            n++;
        }
    }
#endif
    if (n == 0 && max > 0) {
        strncpy(names[0], "HarmonyOS Sans TC", 127);
        names[0][127] = 0;
        n = 1;
    }
    return n;
}
/* ============================================================ 公开包装 */

poe_file_rec *find_file_pub(poe_index *idx, const char *path) {
    return find_file(idx, path);
}
uint8_t *build_bundle_pub(const uint8_t *content, size_t len,
                          int32_t compressor, size_t *out_len) {
    return build_bundle(content, len, compressor, out_len);
}
uint8_t *serialize_index_pub(poe_index *idx, size_t *out_len) {
    return serialize_index(idx, out_len);
}
bool grow_pub(void **arr, int *cap, int need, size_t elemsize) {
    return grow(arr, cap, need, elemsize);
}

/* ============================================================ 备份 / 回退 */
/*
 * 目录布局:
 *   <dir>\_poe_patch_backup\
 *       _.index.bin.orig      原始索引
 *       LibGGPK3\ 0.bundle.bin.orig   若有旧的 LibGGPK3 (二次备份)
 *       info.txt              备份信息
 *
 * 回退:
 *   1. 还原 _.index.bin.orig  -> <dir>\_.index.bin
 *   2. 删除 <dir>\LibGGPK3 目录 (补丁新建的)
 *   3. 若存在 0.bundle.bin.orig, 还原它
 *   4. 删除备份目录
 */

#define POE_BACKUP_DIR "_poe_patch_backup"

static bool ensure_dir_a(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        return true;
    return CreateDirectoryA(path, NULL) != 0;
#else
    return mkdir(path, 0755) == 0;
#endif
}

static bool file_exists_a(const char *path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
#endif
}

static bool copy_file_a(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { poe_set_error("无法打开 %s", src); return false; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); poe_set_error("无法创建 %s", dst); return false; }
    char buf[65536];
    size_t r;
    bool ok = true;
    while ((r = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, r, out) != r) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    fclose(out);
    if (!ok) { poe_set_error("复制 %s 时出错", src); remove(dst); }
    return ok;
}

bool poe_has_backup(const char *dir) {
    char p[1400];
    snprintf(p, sizeof p, "%s\\%s\\_.index.bin.orig", dir, POE_BACKUP_DIR);
    return file_exists_a(p);
}

bool poe_make_backup(const char *dir, const char *index_path) {
    if (poe_has_backup(dir))
        return true;   /* 已有, 保留最早那份 */

    char bdir[1400];
    snprintf(bdir, sizeof bdir, "%s\\%s", dir, POE_BACKUP_DIR);
    if (!ensure_dir_a(bdir)) {
        poe_set_error("无法创建备份目录: %s", bdir);
        return false;
    }

    char dst[1400];
    snprintf(dst, sizeof dst, "%s\\_.index.bin.orig", bdir);
    if (!copy_file_a(index_path, dst))
        return false;

    /* 若已存在旧的 LibGGPK3/0, 一并备份 */
    char lb[1400];
    snprintf(lb, sizeof lb, "%s\\LibGGPK3\\0.bundle.bin", dir);
    if (file_exists_a(lb)) {
        char lbd[1400];
        snprintf(lbd, sizeof lbd, "%s\\0.bundle.bin.orig", bdir);
        if (!copy_file_a(lb, lbd))
            return false;
    }

    char info[1400];
    snprintf(info, sizeof info, "%s\\info.txt", bdir);
    FILE *f = fopen(info, "wb");
    if (f) {
        fprintf(f, "PoE Patch CHT backup\n");
        fprintf(f, "source=%s\n", index_path);
        fclose(f);
    }
    return true;
}

/* 递归删除单层目录内的文件 + 目录本身 (LibGGPK3 只有一级) */
static bool remove_patch_dir(const char *dir) {
    char d[1400];
    snprintf(d, sizeof d, "%s\\LibGGPK3", dir);

#ifdef _WIN32
    DWORD a = GetFileAttributesA(d);
    if (a == INVALID_FILE_ATTRIBUTES)
        return true;   /* 不存在, 视为成功 */
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        DeleteFileA(d);
        return true;
    }
    char pat[1400];
    snprintf(pat, sizeof pat, "%s\\*", d);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            char f[1600];
            snprintf(f, sizeof f, "%s\\%s", d, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryA(f);      /* 只支持空子目录 */
            else
                DeleteFileA(f);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryA(d) != 0;
#else
    return true;
#endif
}

bool poe_rollback(const char *dir, const char *index_path) {
    char bdir[1400];
    snprintf(bdir, sizeof bdir, "%s\\%s", dir, POE_BACKUP_DIR);

    char src[1400];
    snprintf(src, sizeof src, "%s\\_.index.bin.orig", bdir);
    if (!file_exists_a(src)) {
        poe_set_error("没有可用的备份 (未找到 %s)", src);
        return false;
    }

    /* 1. 还原索引 */
    if (!copy_file_a(src, index_path)) {
        /* copy_file_a 已写入具体原因, 这里补充上下文但不覆盖它 */
        char sub[900];
        snprintf(sub, sizeof sub, "%s", poe_last_error);
        poe_set_error("还原索引失败 (%s): %s", index_path, sub);
        return false;
    }
    printf("  [回退] 索引已还原\n");

    /* 2. 处理 LibGGPK3 */
    char lbd[1400];
    snprintf(lbd, sizeof lbd, "%s\\0.bundle.bin.orig", bdir);
    if (file_exists_a(lbd)) {
        /* 有旧备份 -> 先还原目录和文件, 再覆盖 */
        char ldir[1400], lfile[1400];
        snprintf(ldir, sizeof ldir, "%s\\LibGGPK3", dir);
        ensure_dir_a(ldir);
        snprintf(lfile, sizeof lfile, "%s\\0.bundle.bin", ldir);
        if (!copy_file_a(lbd, lfile)) {
            char sub[900];
            snprintf(sub, sizeof sub, "%s", poe_last_error);
            poe_set_error("还原旧 bundle 失败 (%s): %s", lfile, sub);
            return false;
        }
        printf("  [回退] 旧 LibGGPK3/0 已还原\n");
    } else {
        if (remove_patch_dir(dir))
            printf("  [回退] LibGGPK3 目录已删除\n");
        else
            printf("  [警告] LibGGPK3 目录删除失败 (可能有残留)\n");
    }

    /* 3. 删除备份目录内容 */
    remove(src);
    remove(lbd);
    char info[1400];
    snprintf(info, sizeof info, "%s\\info.txt", bdir);
    remove(info);
#ifdef _WIN32
    RemoveDirectoryA(bdir);
#else
    rmdir(bdir);
#endif
    return true;
}

bool poe_discard_backup(const char *dir) {
    char bdir[1400];
    snprintf(bdir, sizeof bdir, "%s\\%s", dir, POE_BACKUP_DIR);

    char p[1400];
    snprintf(p, sizeof p, "%s\\_.index.bin.orig", bdir);
    remove(p);
    snprintf(p, sizeof p, "%s\\0.bundle.bin.orig", bdir);
    remove(p);
    snprintf(p, sizeof p, "%s\\info.txt", bdir);
    remove(p);
#ifdef _WIN32
    RemoveDirectoryA(bdir);
#else
    rmdir(bdir);
#endif
    return true;
}