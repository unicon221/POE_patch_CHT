/*
 * poe_patch_gui.c - Path of Exile 繁中语系 + 字体 修改工具 (CUI 界面)
 *
 * 界面布局:
 *   顶部: 索引文件路径 + [选择文件...]
 *   中部: 字体下拉菜单
 *   下部: [修改字体] [修改语言到繁体中文] [应用全部修改]
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cui/cui.h>
#include <cui/cui_app.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#include "poe_core.h"

/* ============================================================ 状态 */

typedef struct {
    char index_path[1024];
    char font[128];
    int  font_sel;

    char fonts[24][128];
    const char *font_ptrs[24];
    int  font_count;

    char status[512];
    int  status_kind;   /* 0=说明 1=成功 2=错误 */

    bool checked;
    bool lang_applied;
    bool font_ok;
    char cur_font[128];
    char fr_name[128], fr_native[128];
    char tc_name[128], tc_native[128];
} app_state;

static app_state S;

static void set_status(const char *msg, int kind) {
    strncpy(S.status, msg, sizeof S.status - 1);
    S.status[sizeof S.status - 1] = 0;
    S.status_kind = kind;
}

static void load_fonts(void) {
    S.font_count = poe_enum_fonts(S.fonts, 24);
    S.font_sel = 0;
    for (int i = 0; i < S.font_count; i++) {
        S.font_ptrs[i] = S.fonts[i];
        if (strcmp(S.fonts[i], S.font) == 0) S.font_sel = i;
    }
}

/* 自动探测默认索引路径 */
static void detect_default_path(void) {
    static const char *cands[] = {
        "D:\\Games\\Steam\\steamapps\\common\\Path of Exile\\Bundles2\\_.index.bin",
        "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Path of Exile\\Bundles2\\_.index.bin",
        "C:\\Program Files\\Steam\\steamapps\\common\\Path of Exile\\Bundles2\\_.index.bin",
        "D:\\Steam\\steamapps\\common\\Path of Exile\\Bundles2\\_.index.bin",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        FILE *f = fopen(cands[i], "rb");
        if (f) {
            fclose(f);
            strncpy(S.index_path, cands[i], sizeof S.index_path - 1);
            return;
        }
    }
}

/* ============================================================ 检查 */

static bool do_check(void) {
    S.checked = false;
    if (S.index_path[0] == 0) {
        set_status("请先选择 _.index.bin 文件", 2);
        return false;
    }
    FILE *probe = fopen(S.index_path, "rb");
    if (!probe) {
        set_status("索引文件不存在或无法读取", 2);
        return false;
    }
    fclose(probe);

    poe_index idx;
    if (!poe_index_open(&idx, S.index_path)) {
        char m[512];
        snprintf(m, sizeof m, "打开索引失败: %s", poe_last_error);
        set_status(m, 2);
        return false;
    }

    S.lang_applied = false;
    S.font_ok = false;
    S.cur_font[0] = 0;
    S.fr_name[0] = S.fr_native[0] = S.tc_name[0] = S.tc_native[0] = 0;

    /* 语言 */
    size_t len = 0;
    uint8_t *lang = poe_index_read(&idx, POE_LANG_PATH, &len);
    if (lang) {
        poe_lang_row rows[64];
        int cnt = 0;
        if (poe_lang_parse(lang, len, rows, 64, &cnt)) {
            S.lang_applied = poe_lang_is_applied(rows, cnt);
            for (int i = 0; i < cnt; i++) {
                if (strcmp(rows[i].code, "fr") == 0) {
                    strncpy(S.fr_name, rows[i].name, sizeof S.fr_name - 1);
                    strncpy(S.fr_native, rows[i].native_name, sizeof S.fr_native - 1);
                }
                if (strcmp(rows[i].code, "zh-TW") == 0) {
                    strncpy(S.tc_name, rows[i].name, sizeof S.tc_name - 1);
                    strncpy(S.tc_native, rows[i].native_name, sizeof S.tc_native - 1);
                }
            }
        }
        free(lang);
    }

    /* 字体 */
    uint8_t *xml = poe_index_read(&idx, POE_UI_XML_PATH, &len);
    if (xml) {
        bool same = false;
        if (poe_xml_get_typeface(xml, len, S.cur_font, sizeof S.cur_font, &same))
            S.font_ok = same && strcmp(S.cur_font, S.font) == 0;
        free(xml);
    }

    poe_index_close(&idx);
    S.checked = true;
    return true;
}

/* ============================================================ 应用 */

/* what: 1=字体 2=语言 3=全部 */
static void do_apply(int what) {
    if (!do_check()) return;

    if ((what & 1) && !S.font_ok && (what & 2) && !S.lang_applied) {
        /* 两项都需要 */
    } else if ((what & 1) && S.font_ok && !(what & 2)) {
        set_status("字体已是目标字体，无需修改", 1);
        return;
    } else if ((what & 2) && S.lang_applied && !(what & 1)) {
        set_status("语言已是繁体中文，无需修改", 1);
        return;
    }

    poe_index idx;
    if (!poe_index_open(&idx, S.index_path)) {
        char m[512];
        snprintf(m, sizeof m, "打开索引失败: %s", poe_last_error);
        set_status(m, 2);
        return;
    }

    poe_write_item items[2];
    int ni = 0;
    uint8_t *owned[2] = { NULL, NULL };

    /* 字体 */
    if ((what & 1) && !S.font_ok) {
        size_t len = 0;
        uint8_t *xml = poe_index_read(&idx, POE_UI_XML_PATH, &len);
        if (!xml) {
            snprintf(S.status, sizeof S.status, "读取 XML 失败: %s", poe_last_error);
            S.status_kind = 2;
            poe_index_close(&idx);
            return;
        }
        size_t olen = 0;
        uint8_t *nx = poe_xml_set_typeface(xml, len, S.font, &olen);
        free(xml);
        if (!nx) {
            snprintf(S.status, sizeof S.status, "生成 XML 失败: %s", poe_last_error);
            S.status_kind = 2;
            poe_index_close(&idx);
            return;
        }
        items[ni].path = POE_UI_XML_PATH;
        items[ni].data = nx;
        items[ni].len = olen;
        owned[ni] = nx;
        ni++;
    }

    /* 语言 */
    if ((what & 2) && !S.lang_applied) {
        size_t len = 0;
        uint8_t *lang = poe_index_read(&idx, POE_LANG_PATH, &len);
        if (!lang) {
            for (int i = 0; i < ni; i++) free(owned[i]);
            snprintf(S.status, sizeof S.status, "读取 Languages.dat 失败: %s", poe_last_error);
            S.status_kind = 2;
            poe_index_close(&idx);
            return;
        }
        size_t olen = 0;
        uint8_t *nl = poe_lang_apply(lang, len, &olen);
        free(lang);
        if (!nl) {
            for (int i = 0; i < ni; i++) free(owned[i]);
            snprintf(S.status, sizeof S.status, "生成 Languages.dat 失败: %s", poe_last_error);
            S.status_kind = 2;
            poe_index_close(&idx);
            return;
        }
        items[ni].path = POE_LANG_PATH;
        items[ni].data = nl;
        items[ni].len = olen;
        owned[ni] = nl;
        ni++;
    }

    if (ni == 0) {
        poe_index_close(&idx);
        set_status("无需修改，一切就绪。", 1);
        do_check();
        return;
    }

    bool ok = poe_write_back(&idx, items, ni);
    char errbuf[512];
    strncpy(errbuf, poe_last_error, sizeof errbuf - 1);
    errbuf[sizeof errbuf - 1] = 0;

    for (int i = 0; i < ni; i++) free(owned[i]);
    poe_index_close(&idx);

    if (ok) {
        char m[256];
        snprintf(m, sizeof m, "成功！已修改 %d 项。游戏内选择「法语」即可显示繁体中文。", ni);
        set_status(m, 1);
    } else {
        char m[600];
        snprintf(m, sizeof m, "写回失败: %s", errbuf);
        set_status(m, 2);
    }
    do_check();
}

/* 打开系统文件对话框 */
static void pick_index_file(void) {
    static char buf[1024];
    strncpy(buf, S.index_path, sizeof buf - 1);
    if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){
            .kind = CUI_FILE_OPEN,
            .title = "选择 _.index.bin",
            .filter_name = "索引文件 (*.bin)",
            .filter = "bin",
            .default_name = "_.index.bin",
        }, buf, sizeof buf)) {
        strncpy(S.index_path, buf, sizeof S.index_path - 1);
        S.checked = false;
        do_check();
    }
}

/* ============================================================ 字体加载 */

#ifdef _WIN32
/* 从字体族名取得 .ttf/.ttc 文件路径 (取 nIndex 条目) */
static bool font_file_path(const char *family, char *out, size_t cap) {
    static char all[32768];
    char win[512];
    /* 转 UTF-16 再转回 ANSI 不需要: 用宽字符 API 更稳 */
    HKEY hk;
    DWORD idx = 0;
    char value[512];
    DWORD vlen = sizeof value, vtype;
    DWORD dataLen, dlen;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        while (true) {
            vlen = sizeof value; dataLen = sizeof all; dlen = sizeof all;
            if (RegEnumValueA(hk, idx++, value, &vlen, NULL, &vtype,
                              (BYTE*)all, &dlen) != ERROR_SUCCESS)
                break;
            /* 值名形如 "Microsoft YaHei Regular (TrueType)" */
            if (strncmp(value, family, strlen(family)) != 0) continue;
            snprintf(win, sizeof win, "%s", all);
            if (win[0]) {
                RegCloseKey(hk);
                /* 相对路径补系统字体目录 */
                if (win[1] == ':') snprintf(out, cap, "%s", win);
                else {
                    char windir[MAX_PATH];
                    GetWindowsDirectoryA(windir, sizeof windir);
                    snprintf(out, cap, "%s\\Fonts\\%s", windir, win);
                }
                return true;
            }
        }
        RegCloseKey(hk);
    }
    return false;
}

/* 直接从目录里找字体文件 */
static bool find_font_file(const char *filename, char *out, size_t cap) {
    char windir[MAX_PATH];
    GetWindowsDirectoryA(windir, sizeof windir);
    snprintf(out, cap, "%s\\Fonts\\%s", windir, filename);
    FILE *f = fopen(out, "rb");
    if (f) { fclose(f); return true; }
    /* 用户字体目录 */
    char local[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof local)) {
        char alt[MAX_PATH * 2];
        snprintf(alt, sizeof alt, "%s\\Microsoft\\Windows\\Fonts\\%s", local, filename);
        f = fopen(alt, "rb");
        if (f) { fclose(f); snprintf(out, cap, "%s", alt); return true; }
    }
    return false;
}
#endif

/* 加载中文字体作为 fallback, 解决中文不显示的问题 */
static void setup_cjk_font(cui_ctx *ui) {
#ifdef _WIN32
    /* 按优先级尝试的字体文件 */
    static const char *files[] = {
        "msyh.ttc",    /* 微软雅黑 */
        "msyhbd.ttc",
        "msjh.ttc",    /* 微软正黑体 */
        "simsun.ttc",  /* 宋体 */
        "simhei.ttf",  /* 黑体 */
        "Deng.ttf",    /* 等线 */
        NULL
    };
    char path[1024];
    for (int i = 0; files[i]; i++) {
        if (!find_font_file(files[i], path, sizeof path)) continue;
        cui_font f = cui_font_load_file(ui, path);
        if (f < 0) continue;
        /* 给所有内置角色加 CJK 回退 */
        for (int r = CUI_FONT_BODY; r <= CUI_FONT_MONO; r++)
            cui_font_add_fallback(ui, r, f);
        fprintf(stderr, "[font] CJK fallback: %s\n", path);
        return;
    }
    /* 退而用注册表按族名找 */
    static const char *fams[] = { "Microsoft YaHei", "Microsoft JhengHei", "SimSun", NULL };
    for (int i = 0; fams[i]; i++) {
        if (!font_file_path(fams[i], path, sizeof path)) continue;
        cui_font f = cui_font_load_file(ui, path);
        if (f < 0) continue;
        for (int r = CUI_FONT_BODY; r <= CUI_FONT_MONO; r++)
            cui_font_add_fallback(ui, r, f);
        fprintf(stderr, "[font] CJK fallback: %s (%s)\n", path, fams[i]);
        return;
    }
    fprintf(stderr, "[font] 警告: 未找到任何中文字体, 中文将无法显示\n");
#endif
}

/* ============================================================ UI */

static void frame(cui_ctx *ui, void *user) {
    (void)user;

    cui_column(ui, .pad = 22, .gap = 12,
               .bg = CUI_COLOR_BACKGROUND, .width = CUI_FILL, .height = CUI_FILL) {

        cui_row(ui, .gap = 10) {
            cui_heading(ui, "Path of Exile 繁中语系 + 字体", 3);
        }

        /* ---------- 1. 路径 ---------- */
        cui_card(ui, .gap = 8) {
            cui_label(ui, "游戏索引文件");
            if (S.index_path[0])
                cui_text(ui, S.index_path, .size = 13);
            else
                cui_text(ui, "(未选择)", .color = CUI_COLOR_DANGER);
            cui_row(ui, .gap = 8) {
                if (cui_button(ui, "选择文件...", .variant = CUI_BUTTON_SECONDARY))
                    pick_index_file();
                if (cui_button(ui, "重新检查"))
                    do_check();
            }
        }

        /* ---------- 2. 字体 ---------- */
        cui_select(ui, "font", S.font_ptrs, S.font_count, &S.font_sel,
                   .label = "字体");
        if (S.font_sel >= 0 && S.font_sel < S.font_count) {
            if (strcmp(S.font, S.fonts[S.font_sel]) != 0) {
                strncpy(S.font, S.fonts[S.font_sel], sizeof S.font - 1);
                S.font[sizeof S.font - 1] = 0;
                S.checked = false;
            }
        }

        /* ---------- 3. 状态 ---------- */
        if (S.checked) {
            cui_row(ui, .gap = 8) {
                if (S.lang_applied)
                    cui_badge(ui, "语言已应用", .tone = CUI_TONE_SUCCESS, .dot = true);
                else
                    cui_badge(ui, "语言未应用", .tone = CUI_TONE_DANGER, .dot = true);
                if (S.font_ok)
                    cui_badge(ui, "字体已应用", .tone = CUI_TONE_SUCCESS, .dot = true);
                else
                    cui_badge(ui, "字体未应用", .tone = CUI_TONE_DANGER, .dot = true);
            }
            if (S.fr_name[0] || S.tc_name[0]) {
                cui_secondary(ui, "语言行:");
                cui_textf(ui, "fr    -> %s  (%s)", S.fr_name, S.fr_native);
                cui_textf(ui, "zh-TW -> %s  (%s)", S.tc_name, S.tc_native);
            }
        }

        /* ---------- 4. 按钮 ---------- */
        cui_row(ui, .gap = 10) {
            if (cui_button(ui, "修改字体", .icon = CUI_ICON_TYPE))
                do_apply(1);
            if (cui_button(ui, "修改语言到繁体中文", .icon = CUI_ICON_GLOBE))
                do_apply(2);
            if (cui_button(ui, "应用全部修改",
                           .variant = CUI_BUTTON_PRIMARY,
                           .icon = CUI_ICON_CHECK))
                do_apply(3);
        }

        /* ---------- 5. 提示 ---------- */
        if (S.status[0]) {
            if (S.status_kind == 1)
                cui_text(ui, S.status, .color = CUI_COLOR_SUCCESS);
            else if (S.status_kind == 2)
                cui_text(ui, S.status, .color = CUI_COLOR_DANGER);
            else
                cui_secondary(ui, S.status);
        }
    }
}

int main(int argc, char **argv) {
    memset(&S, 0, sizeof S);
    strncpy(S.font, "HarmonyOS Sans TC", sizeof S.font - 1);

    if (argc > 1)
        strncpy(S.index_path, argv[1], sizeof S.index_path - 1);
    else
        detect_default_path();

    load_fonts();

    if (!poe_oodle_init("oo2core.dll"))
        set_status("找不到 oo2core.dll（必须与程序放在同一目录）", 2);

    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "PoE 繁中语系 + 字体",
        .width = 760,
        .height = 600,
        .min_width = 600,
        .min_height = 500,
    });
    if (!app) {
        fprintf(stderr, "无法创建窗口\n");
#ifdef _WIN32
        MessageBoxA(NULL,
            "无法创建窗口。\n\n可能是图形环境不可用，或缺少 OpenGL 支持。",
            "PoE 繁中语系 + 字体", MB_ICONERROR | MB_OK);
#endif
        return 1;
    }

    if (S.index_path[0]) do_check();

    /* 首帧前加载中文字体回退，否则中文全部显示为空白 */
    cui_ctx *ui = cui_app_ui(app);
    if (ui) setup_cjk_font(ui);

    while (cui_app_frame(app)) {
        frame(cui_app_ui(app), NULL);
    }

    cui_app_destroy(app);
    poe_oodle_shutdown();
    return 0;
}
