/*
 * poe_core.h - Path of Exile Bundles2 读写核心 (移植自 Python 版)
 *
 * 支持:
 *   - 读取 _.index.bin (本身是 format3 bundle)
 *   - 按路径查找并读取文件
 *   - 修改 Languages.dat / UISettings.*.xml 并写回
 *
 * 依赖 oo2core.dll (Oodle 解压/压缩)
 */
#ifndef POE_CORE_H
#define POE_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define POE_CHUNK_SIZE   262144
#define POE_INDEX_NAME   "_.index.bin"
#define POE_CUSTOM_PREFIX "LibGGPK3/"

#define POE_LANG_PATH  "data/languages.dat"
#define POE_UI_XML_PATH "metadata/ui/uisettings.traditional chinese.xml"

#define POE_MAX_BUNDLES 200000
#define POE_MAX_FILES   4000000

typedef struct {
    char    *path;          /* bundle 相对路径 (无 .bundle.bin) */
    int32_t  uncompressed;
} poe_bundle_rec;

typedef struct {
    uint64_t hash;
    int32_t  bundle;
    int32_t  offset;
    int32_t  size;
} poe_file_rec;

typedef struct {
    uint64_t hash;
    int32_t  offset;
    int32_t  size;
    int32_t  recsize;
} poe_dir_rec;

typedef struct {
    char         dir[1024];        /* Bundles2 目录 */
    char         index_path[1200];
    int32_t      compressor;
    int32_t      chunk_size;

    poe_bundle_rec *bundles;
    int            nbundles, cbundles;

    poe_file_rec   *files;
    int             nfiles, cfiles;

    poe_dir_rec    *dirs;
    int             ndirs, cdirs;

    uint8_t        *dir_bundle_data;
    size_t          dir_bundle_len;

    bool            is_murmur;
} poe_index;

/* ---------- 错误信息 ---------- */
extern char poe_last_error[1024];
void poe_set_error(const char *fmt, ...);

/* ---------- Oodle ---------- */
bool poe_oodle_init(const char *dll_path);
void poe_oodle_shutdown(void);
bool poe_oodle_load_from_memory(const void *data, size_t len);

/* ---------- 索引 ---------- */
/* index_path: 指向 _.index.bin */
bool poe_index_open(poe_index *idx, const char *index_path);
void poe_index_close(poe_index *idx);

/* 读取文件内容, 返回 malloc 的缓冲区 (调用者 free), 失败返回 NULL */
uint8_t *poe_index_read(poe_index *idx, const char *path, size_t *out_len);

/* 判断文件是否存在 */
bool poe_index_has(poe_index *idx, const char *path);

/* ---------- Languages.dat ---------- */
typedef struct {
    char  name[128];
    char  native_name[128];
    char  code[64];
} poe_lang_row;

/* 解析 Languages.dat (UTF-8 输出) */
bool poe_lang_parse(const uint8_t *data, size_t len,
                    poe_lang_row *rows, int max_rows, int *out_count);

/* 检查是否已应用繁中到法语入口 */
bool poe_lang_is_applied(const poe_lang_row *rows, int count);

/* 生成修改后的 Languages.dat, 返回 malloc 缓冲 */
uint8_t *poe_lang_apply(const uint8_t *data, size_t len, size_t *out_len);

/* ---------- UISettings XML ---------- */
/* 获取当前 typeface (取第一个), 报告是否全部一致 */
bool poe_xml_get_typeface(const uint8_t *data, size_t len,
                          char *face, size_t face_cap, bool *all_same);
/* 替换所有 typeface, 返回 malloc 缓冲 */
uint8_t *poe_xml_set_typeface(const uint8_t *data, size_t len,
                              const char *face, size_t *out_len);

/* ---------- 写回 ---------- */
/* items: 路径+内容对. 追加到 LibGGPK3 bundle 并重写索引 */
typedef struct {
    const char    *path;
    const uint8_t *data;
    size_t         len;
} poe_write_item;

bool poe_write_back(poe_index *idx, const poe_write_item *items, int n);

/* ---------- 系统字体 ---------- */
/* 枚举候选字体 (常见中文 + HarmonyOS 系列), 返回数量 */
int poe_enum_fonts(char names[][128], int max);

#endif
