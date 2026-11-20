/*
 * poe_ggpk.h - GGPK v3 容器只读访问 (独立客户端 Content.ggpk)
 *
 * 用于从 GGG 官方独立客户端的 Content.ggpk 中取出 Bundles2 下的 bundle 文件,
 * 之后复用与 Steam 版相同的 bundle 解析逻辑。
 *
 * 格式 (参照 poedb.tw/us/GGPK):
 *   GGPK: uint32 Length | "GGPK" | uint32 Version | uint64 root | uint64 first_free
 *   PDIR: uint32 Length | "PDIR" | uint32 NameLength | uint32 TotalEntries
 *         | byte[32] SHA256 | wchar_t[NameLength] Name | DirectoryEntry[TotalEntries]
 *   FILE: uint32 Length | "FILE" | uint32 NameLength | byte[32] SHA256
 *         | wchar_t[NameLength] Name | byte[] Data
 *   DirectoryEntry: int32 nameHash | uint64 offset
 *
 * 只读: 本模块不修改 GGPK。写入仍然通过新建 LibGGPK3 bundle + 重写
 *       Bundles2/_.index.bin 完成 (与 Steam 版策略一致)。
 */
#ifndef POE_GGPK_H
#define POE_GGPK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    FILE   *f;
    int64_t size;
    uint32_t version;
    int64_t root_offset;
    int64_t first_free_offset;
    bool    writable;      /* 是否以读写方式打开 */
} poe_ggpk;

/* 打开 Content.ggpk, 校验魔数 */
bool poe_ggpk_open(poe_ggpk *g, const char *path);
void poe_ggpk_close(poe_ggpk *g);

/* 是否为合法 GGPK 文件 (仅读头部判断, 不打开持久句柄) */
bool poe_is_ggpk_file(const char *path);

/*
 * 在 GGPK 的文件树中按路径查找文件 (大小写不敏感, 用 '/' 分隔)。
 * 例如: "Bundles2/_.index.bin"
 * 找到返回 true, 并给出记录偏移与数据区间。
 */
typedef struct {
    int64_t record_offset;   /* FILE 记录起始偏移 */
    int64_t data_offset;     /* 数据起始绝对偏移 */
    int64_t data_length;     /* 数据长度 */
    char    name[256];
} poe_ggpk_entry;

bool poe_ggpk_find(poe_ggpk *g, const char *path, poe_ggpk_entry *out);

/* 读取整个文件数据 (malloc, 调用者 free) */
uint8_t *poe_ggpk_read(poe_ggpk *g, const char *path, size_t *out_len);

/* 列出某个目录下的条目 (用于诊断/调试) */
typedef void (*poe_ggpk_list_cb)(void *user, char type, const char *name, int64_t size);
bool poe_ggpk_list(poe_ggpk *g, const char *dir_path, poe_ggpk_list_cb cb, void *user);

/*
 * 写入: 在 GGPK 中创建或替换一个文件, 内容为 data/len。
 *
 * 策略 (参照 LibGGPK3 的 DirectoryRecord.AddFile / WriteWithNewLength):
 *   1. 解析路径, 逐级查找目录 (可自动创建中间目录)
 *   2. 目标文件已存在 -> 追加新的 FILE 记录, 更新目录条目 offset
 *      目标文件不存在 -> 追加新 FILE 记录, 在父目录插入新条目
 *   3. 目录条目变化 -> 把整个 PDIR 记录追加到文件末尾, 更新其父目录的 offset
 *   4. 逐级向上重算 SHA256 (父目录哈希 = 子节点哈希的组合)
 *   5. 更新 GGPK 头部记录长度
 *
 * 所有记录只追加, 不移动任何现有数据。旧记录成为垃圾但不影响游戏。
 */
bool poe_ggpk_write_file(poe_ggpk *g, const char *path,
                         const uint8_t *data, size_t len);

/* 校验写入结果: 重新读取并与期望内容比对 */
bool poe_ggpk_verify(const char *ggpk_path, const char *path,
                     const uint8_t *expect, size_t expect_len);

#endif
