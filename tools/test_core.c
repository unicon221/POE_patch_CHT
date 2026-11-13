/*
 * test_core.c - 核心逻辑测试 (无 GUI)
 *
 * 验证:
 *   1. 打开索引、读取 Languages.dat
 *   2. 解析语言行
 *   3. 生成修改版并比对
 *   4. 读取 XML 并替换字体
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "poe_core.h"

static const char *IDX =
    "D:\\Games\\Steam\\steamapps\\common\\Path of Exile\\Bundles2\\_.index.bin";

int main(void) {
    printf("=== C 核心测试 ===\n\n");

    if (!poe_oodle_init("oo2core.dll")) {
        printf("[FAIL] Oodle: %s\n", poe_last_error);
        return 1;
    }
    printf("[OK] Oodle 载入\n");

    poe_index idx;
    if (!poe_index_open(&idx, IDX)) {
        printf("[FAIL] 索引: %s\n", poe_last_error);
        return 1;
    }
    printf("[OK] 索引: %d bundles, %d files, %d dirs, murmur=%d\n",
           idx.nbundles, idx.nfiles, idx.ndirs, idx.is_murmur);

    /* ---- Languages.dat ---- */
    size_t len = 0;
    uint8_t *lang = poe_index_read(&idx, POE_LANG_PATH, &len);
    if (!lang) {
        printf("[FAIL] 读 Languages.dat: %s\n", poe_last_error);
        poe_index_close(&idx);
        return 1;
    }
    printf("[OK] Languages.dat: %zu 字节\n", len);

    poe_lang_row rows[64];
    int cnt = 0;
    if (!poe_lang_parse(lang, len, rows, 64, &cnt)) {
        printf("[FAIL] 解析: %s\n", poe_last_error);
        free(lang); poe_index_close(&idx);
        return 1;
    }
    printf("[OK] 解析出 %d 行:\n", cnt);
    for (int i = 0; i < cnt; i++)
        printf("      [%2d] %-22s %-20s %s\n", i, rows[i].name, rows[i].native_name, rows[i].code);

    printf("\n      is_applied = %d\n", poe_lang_is_applied(rows, cnt));

    size_t olen = 0;
    uint8_t *nl = poe_lang_apply(lang, len, &olen);
    if (!nl) {
        printf("[FAIL] 生成: %s\n", poe_last_error);
    } else {
        printf("[OK] 生成修改版: %zu 字节 (原 %zu)\n", olen, len);
        poe_lang_row rows2[64];
        int cnt2 = 0;
        if (poe_lang_parse(nl, olen, rows2, 64, &cnt2)) {
            printf("      修改后:\n");
            for (int i = 0; i < cnt2; i++)
                printf("      [%2d] %-22s %-20s %s\n", i, rows2[i].name, rows2[i].native_name, rows2[i].code);
            printf("      is_applied = %d\n", poe_lang_is_applied(rows2, cnt2));
        } else {
            printf("[FAIL] 重新解析: %s\n", poe_last_error);
        }
        /* 往返: 再应用一次应回到原始 */
        size_t olen2 = 0;
        uint8_t *back = poe_lang_apply(nl, olen, &olen2);
        if (back) {
            printf("      二次应用回到原始: %s (len %zu vs %zu)\n",
                   (olen2 == len && memcmp(back, lang, len) == 0) ? "是" : "否", olen2, len);
            free(back);
        }
        free(nl);
    }
    free(lang);

    /* ---- XML ---- */
    uint8_t *xml = poe_index_read(&idx, POE_UI_XML_PATH, &len);
    if (!xml) {
        printf("[FAIL] 读 XML: %s\n", poe_last_error);
    } else {
        printf("\n[OK] XML: %zu 字节\n", len);
        char face[128] = {0};
        bool same = false;
        if (poe_xml_get_typeface(xml, len, face, sizeof face, &same))
            printf("      当前 typeface: %s (全部一致: %d)\n", face, same);
        else
            printf("      未找到 typeface\n");

        size_t xlen = 0;
        uint8_t *nx = poe_xml_set_typeface(xml, len, "HarmonyOS Sans TC", &xlen);
        if (nx) {
            char f2[128] = {0};
            bool s2 = false;
            poe_xml_get_typeface(nx, xlen, f2, sizeof f2, &s2);
            printf("      替换后: %s (全部一致: %d), 大小 %zu -> %zu\n", f2, s2, len, xlen);
            printf("      BOM: %02X %02X\n", nx[0], nx[1]);
            free(nx);
        } else {
            printf("[FAIL] 替换字体: %s\n", poe_last_error);
        }
        free(xml);
    }

    /* ---- 字体枚举 ---- */
    char fonts[24][128];
    int nf = poe_enum_fonts(fonts, 24);
    printf("\n[OK] 候选字体 (%d):\n", nf);
    for (int i = 0; i < nf; i++) printf("      %s\n", fonts[i]);

    poe_index_close(&idx);
    poe_oodle_shutdown();
    printf("\n=== 完成 ===\n");
    return 0;
}
