/*
 * test_e2e.c - 端到端测试: 真正写回并验证
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "poe_core.h"

static const char *IDX =
    "D:\\Games\\Steam\\steamapps\\common\\Path of Exile\\Bundles2\\_.index.bin";

int main(int argc, char **argv) {
    bool do_write = (argc > 1 && strcmp(argv[1], "write") == 0);

    printf("=== C 版端到端测试 ===\n\n");
    if (!poe_oodle_init("oo2core.dll")) {
        printf("[FAIL] Oodle: %s\n", poe_last_error);
        return 1;
    }

    poe_index idx;
    if (!poe_index_open(&idx, IDX)) {
        printf("[FAIL] 索引: %s\n", poe_last_error);
        return 1;
    }
    printf("[OK] %d bundles, %d files, %d dirs\n", idx.nbundles, idx.nfiles, idx.ndirs);

    /* 检查当前状态 */
    size_t len = 0;
    uint8_t *lang = poe_index_read(&idx, POE_LANG_PATH, &len);
    poe_lang_row rows[64];
    int cnt = 0;
    bool applied = false;
    if (lang) {
        if (poe_lang_parse(lang, len, rows, 64, &cnt))
            applied = poe_lang_is_applied(rows, cnt);
        free(lang);
    }
    printf("[OK] 语言已应用: %d\n", applied);

    uint8_t *xml = poe_index_read(&idx, POE_UI_XML_PATH, &len);
    char face[128] = {0};
    bool same = false;
    if (xml) {
        poe_xml_get_typeface(xml, len, face, sizeof face, &same);
        free(xml);
    }
    printf("[OK] 当前字体: %s\n", face);

    if (!do_write) {
        printf("\n(只读模式, 加 write 参数执行写回)\n");
        poe_index_close(&idx);
        return 0;
    }

    /* ---- 写回 ---- */
    printf("\n--- 执行写回 ---\n");
    poe_write_item items[2];
    int ni = 0;
    uint8_t *owned[2] = {0, 0};

    if (!(same && strcmp(face, "HarmonyOS Sans TC") == 0)) {
        uint8_t *x = poe_index_read(&idx, POE_UI_XML_PATH, &len);
        if (x) {
            size_t ol = 0;
            uint8_t *nx = poe_xml_set_typeface(x, len, "HarmonyOS Sans TC", &ol);
            free(x);
            if (nx) {
                items[ni].path = POE_UI_XML_PATH;
                items[ni].data = nx;
                items[ni].len = ol;
                owned[ni++] = nx;
                printf("  准备: 字体 -> HarmonyOS Sans TC (%zu 字节)\n", ol);
            }
        }
    }

    if (!applied) {
        uint8_t *l = poe_index_read(&idx, POE_LANG_PATH, &len);
        if (l) {
            size_t ol = 0;
            uint8_t *nl = poe_lang_apply(l, len, &ol);
            free(l);
            if (nl) {
                items[ni].path = POE_LANG_PATH;
                items[ni].data = nl;
                items[ni].len = ol;
                owned[ni++] = nl;
                printf("  准备: 语言 -> 繁中挂法语 (%zu 字节)\n", ol);
            }
        }
    }

    if (ni == 0) {
        printf("  无需修改\n");
        poe_index_close(&idx);
        return 0;
    }

    bool ok = poe_write_back(&idx, items, ni);
    printf("  写回结果: %s\n", ok ? "成功" : poe_last_error);
    for (int i = 0; i < ni; i++) free(owned[i]);
    poe_index_close(&idx);

    if (!ok) return 1;

    /* ---- 重新打开验证 ---- */
    printf("\n--- 验证 ---\n");
    poe_index idx2;
    if (!poe_index_open(&idx2, IDX)) {
        printf("[FAIL] 重开: %s\n", poe_last_error);
        return 1;
    }
    uint8_t *l2 = poe_index_read(&idx2, POE_LANG_PATH, &len);
    poe_lang_row r2[64];
    int c2 = 0;
    if (l2 && poe_lang_parse(l2, len, r2, 64, &c2)) {
        bool ap2 = poe_lang_is_applied(r2, c2);
        printf("  语言已应用: %d\n", ap2);
        for (int i = 0; i < c2; i++)
            if (strcmp(r2[i].code, "fr") == 0 || strcmp(r2[i].code, "zh-TW") == 0)
                printf("    %-8s %-22s %s\n", r2[i].code, r2[i].name, r2[i].native_name);
    }
    if (l2) free(l2);

    uint8_t *x2 = poe_index_read(&idx2, POE_UI_XML_PATH, &len);
    if (x2) {
        char f2[128] = {0};
        bool s2 = false;
        poe_xml_get_typeface(x2, len, f2, sizeof f2, &s2);
        printf("  字体: %s (一致: %d)\n", f2, s2);
        free(x2);
    }
    printf("  bundles=%d files=%d dirs=%d\n", idx2.nbundles, idx2.nfiles, idx2.ndirs);
    poe_index_close(&idx2);
    printf("\n[完成]\n");
    return 0;
}
