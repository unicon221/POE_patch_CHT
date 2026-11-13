/*
 * shot.c - 离屏渲染最终界面, 输出 PNG (无需窗口)
 * 复用 poe_patch_gui.c 的界面逻辑: 通过定义 POE_SHOT_MODE 让 GUI 文件提供 frame()
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cui/cui.h>
#include <cui/cui_render.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#include "poe_core.h"

/* ---- 与主程序一致的界面 ---- */
static char g_index_path[1024] =
    "D:\\Games\\Steam\\steamapps\\common\\Path of Exile\\Bundles2\\_.index.bin";
static char g_font[128] = "HarmonyOS Sans TC";
static int  g_font_sel = 0;
static char g_fonts[24][128];
static const char *g_font_ptrs[24];
static int  g_font_count = 0;

static bool g_lang_applied = true;
static bool g_font_ok = false;
static char g_fr_name[128] = "Traditional Chinese", g_fr_native[128] = "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87";
static char g_tc_name[128] = "French", g_tc_native[128] = "Fran\xc3\xa7" "ais";
static const char *g_status = "\xe6\x97\xa0\xe9\x9c\x80\xe4\xbf\xae\xe6\x94\xb9\xef\xbc\x8c\xe4\xb8\x80\xe5\x88\x87\xe5\xb0\xb1\xe7\xbb\xaa\xe3\x80\x82";
static int g_status_kind = 1;

static void load_fonts(void) {
    g_font_count = poe_enum_fonts(g_fonts, 24);
    for (int i = 0; i < g_font_count; i++) {
        g_font_ptrs[i] = g_fonts[i];
        if (strcmp(g_fonts[i], g_font) == 0) g_font_sel = i;
    }
}

static void ui_frame(cui_ctx *ui) {
    cui_column(ui, .pad = 22, .gap = 12,
               .bg = CUI_COLOR_BACKGROUND, .width = CUI_FILL, .height = CUI_FILL) {

        cui_row(ui, .gap = 10) {
            cui_heading(ui, "Path of Exile 繁中语系 + 字体", 3);
        }

        cui_card(ui, .gap = 8) {
            cui_label(ui, "游戏索引文件");
            cui_text(ui, g_index_path, .size = 13);
            cui_row(ui, .gap = 8) {
                cui_button(ui, "选择文件...", .variant = CUI_BUTTON_SECONDARY);
                cui_button(ui, "重新检查");
            }
        }

        cui_select(ui, "font", g_font_ptrs, g_font_count, &g_font_sel, .label = "字体");

        cui_row(ui, .gap = 8) {
            if (g_lang_applied)
                cui_badge(ui, "语言已应用", .tone = CUI_TONE_SUCCESS, .dot = true);
            else
                cui_badge(ui, "语言未应用", .tone = CUI_TONE_DANGER, .dot = true);
            if (g_font_ok)
                cui_badge(ui, "字体已应用", .tone = CUI_TONE_SUCCESS, .dot = true);
            else
                cui_badge(ui, "字体未应用", .tone = CUI_TONE_DANGER, .dot = true);
        }

        cui_secondary(ui, "语言行:");
        cui_textf(ui, "fr    -> %s  (%s)", g_fr_name, g_fr_native);
        cui_textf(ui, "zh-TW -> %s  (%s)", g_tc_name, g_tc_native);

        cui_row(ui, .gap = 10) {
            cui_button(ui, "修改字体", .icon = CUI_ICON_TYPE);
            cui_button(ui, "修改语言到繁体中文", .icon = CUI_ICON_GLOBE);
            cui_button(ui, "应用全部修改", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_CHECK);
        }

        if (g_status && g_status[0]) {
            if (g_status_kind == 1) cui_text(ui, g_status, .color = CUI_COLOR_SUCCESS);
            else if (g_status_kind == 2) cui_text(ui, g_status, .color = CUI_COLOR_DANGER);
            else cui_secondary(ui, g_status);
        }
    }
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "ui.png";
    poe_oodle_init("oo2core.dll");
    load_fonts();

    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){
        .renderer = cui_sw_iface(sw),
        .theme = CUI_THEME_LIGHT,
    });
    if (!ui) { printf("cui_create failed\n"); return 1; }

    /* 中文字体回退 */
    {
        static const char *files[] = { "msyh.ttc", "msjh.ttc", "simsun.ttc", "simhei.ttf", NULL };
        char windir[MAX_PATH];
        GetWindowsDirectoryA(windir, sizeof windir);
        for (int i = 0; files[i]; i++) {
            char p[MAX_PATH * 2];
            snprintf(p, sizeof p, "%s\\Fonts\\%s", windir, files[i]);
            FILE *f = fopen(p, "rb");
            if (!f) continue;
            fclose(f);
            cui_font cjk = cui_font_load_file(ui, p);
            if (cjk < 0) continue;
            for (int r = CUI_FONT_BODY; r <= CUI_FONT_MONO; r++)
                cui_font_add_fallback(ui, r, cjk);
            printf("CJK fallback: %s\n", p);
            break;
        }
    }

    float W = 760, H = 600, S = 2;
    cui_set_viewport(ui, W, H, S);
    for (int f = 0; f < 2; f++) {
        cui_begin_frame(ui, 1.0f / 60);
        ui_frame(ui);
        cui_end_frame(ui);
    }

    const cui_draw_data *dd = cui_get_draw_data(ui);
    int w = dd->width_px, h = dd->height_px;
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return 1;
    cui_sw_render(sw, dd, px, w, h, w * 4, CUI_SW_RGBA);
    cui_write_png(out, px, w, h, w * 4);
    printf("vertices=%d indices=%d cmds=%d -> %s (%dx%d)\n",
           dd->vertex_count, dd->index_count, dd->command_count, out, w, h);

    free(px);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    return 0;
}
