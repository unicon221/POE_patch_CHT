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
    char         dir[1024];        /* Bundles2 目录 (Steam) 或 GGPK 所在目录 */
    char         index_path[1200]; /* Steam: _.index.bin 路径; GGPK: Content.ggpk 路径 */
    int32_t      compressor;
    int32_t      chunk_size;

    /* --- 容器模式 --- */
    bool         is_ggpk;          /* true = 独立客户端 Content.ggpk */
    void        *ggpk;             /* poe_ggpk* (仅 is_ggpk 时非 NULL) */

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
/* 打开索引。
 *   - Steam 版: index_path 指向 ...\Bundles2\_.index.bin
 *   - 独立版  : index_path 指向 ...\Content.ggpk
 * 自动识别格式。
 */
bool poe_index_open(poe_index *idx, const char *index_path);
void poe_index_close(poe_index *idx);

/* 自动探测: 给定任意路径 (_.index.bin 或 Content.ggpk 或目录), 找到可用的索引路径 */
bool poe_resolve_target(const char *input, char *out, size_t out_cap, bool *is_ggpk);

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

/* ---------- 备份 / 回退 ---------- */
/* 备份目录: <dir>\_poe_patch_backup
 * 首次修改前保存原始 _.index.bin (Steam) 或 GGPK 结构 (独立版),
 * 回退时还原它们并删除 LibGGPK3 bundle。 */

/* 是否存在可回退的备份 */
bool poe_has_backup(const char *dir);

/* 创建备份 (已存在则保留最早的一份, 不覆盖) */
bool poe_make_backup(const char *dir, const char *index_path);

/* 回退到备份状态。成功后删除备份标记。
 * 返回 false 时 poe_last_error 有原因。 */
bool poe_rollback(const char *dir, const char *index_path);

/* 删除备份 (放弃回退能力) */
bool poe_discard_backup(const char *dir);

/* ---------- 系统字体 ---------- */
/* 枚举候选字体 (常见中文 + HarmonyOS 系列), 返回数量 */
int poe_enum_fonts(char names[][128], int max);

/* ---------- 供外部工具使用的内部函数 ---------- */
typedef struct {
    int32_t uncompressed_size;
    int32_t compressed_size;
    int32_t head_size;
    int32_t compressor;
    int32_t chunk_count;
    int32_t chunk_size;
} bundle_hdr;
uint8_t *bundle_read_all(const char *path, bundle_hdr *out_hdr, size_t *out_len);
poe_file_rec *find_file_pub(poe_index *idx, const char *path);
uint8_t *build_bundle_pub(const uint8_t *content, size_t len,
                          int32_t compressor, size_t *out_len);
uint8_t *serialize_index_pub(poe_index *idx, size_t *out_len);
bool grow_pub(void **arr, int *cap, int need, size_t elemsize);

#endif
