# 构建说明

## 依赖

| 组件 | 说明 |
|---|---|
| **MinGW-w64** | 需要 `x86_64-w64-mingw32-gcc` 和 `windres` |
| **Python 3.8+** | 可选，仅用于生成图标 |
| **CUI** | 必须单独获取（见下） |

### 安装 MinGW-w64

推荐用 winget：

```
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

安装后把 MinGW 的 `bin` 目录加入 `PATH`：

```
%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_*\mingw64\bin
```

验证：

```
x86_64-w64-mingw32-gcc.exe --version
windres.exe --version
```

### 获取 CUI

CUI 不在本仓库内，需要克隆到 `cui_src\CUI-master`：

```
git clone https://github.com/yappologistic/CUI.git cui_src/CUI-master
```

或者下载 zip：

```
curl -L https://github.com/yappologistic/CUI/archive/refs/heads/master.zip -o cui.zip
tar -xf cui.zip
move CUI-master cui_src\CUI-master
```

## 构建

```
build.bat
```

脚本会依次：

1. 如果 `app.ico` 不存在，用 `tools\make_icon.py` 生成
2. 用 `windres` 编译 `src\app.rc` → `app_res.o`
3. 静态链接所有源码 → `poe_patch.exe`

## 手动构建

如果不想用 `build.bat`：

```
windres -i src\app.rc -o app_res.o

x86_64-w64-mingw32-gcc -std=c11 -O2 -mwindows -static -static-libgcc ^
  -Icui_src\CUI-master\include -Icui_src\CUI-master\src ^
  -o poe_patch.exe ^
  src\poe_patch_gui.c src\poe_core.c app_res.o ^
  "cui_src\CUI-master\src\*.c" ^
  "cui_src\CUI-master\src\render\*.c" ^
  "cui_src\CUI-master\src\platform\cui_app.c" ^
  "cui_src\CUI-master\src\platform\cui_dialogs.c" ^
  "cui_src\CUI-master\src\platform\cui_app_win32.c" ^
  -lgdi32 -lopengl32 -lgdiplus -lshell32 -lole32 -luser32 ^
  -lcomdlg32 -luuid -lcomctl32 -lshlwapi -ldwmapi -lwinmm -ladvapi32
```

### 关键编译选项

| 选项 | 作用 |
|---|---|
| `-mwindows` | 窗口子系统，运行时不弹控制台 |
| `-static -static-libgcc` | 静态链接，消除对 `libwinpthread-1.dll` 的依赖 |
| `-Icui_src\CUI-master\src` | CUI 内部头文件（如 `cui_internal.h`） |
| `-ladvapi32` | 读取注册表字体信息 |

## 运行前

把 `oo2core.dll` 放到 `poe_patch.exe` 同目录。获取方式见主 README。

## 测试工具

`tools\` 下的程序用于开发验证：

```
# 核心逻辑测试（只读，不会修改游戏文件）
x86_64-w64-mingw32-gcc -std=c11 -O2 -o test_core.exe tools\test_core.c src\poe_core.c -lgdi32
test_core.exe

# 端到端测试（加 write 参数才会真正写回，需先改 test_e2e.c 里的路径）
x86_64-w64-mingw32-gcc -std=c11 -O2 -o test_e2e.exe tools\test_e2e.c src\poe_core.c -lgdi32
test_e2e.exe          # 只读
test_e2e.exe write    # 执行写回

# 界面离屏截图（不需要窗口，适合 CI）
x86_64-w64-mingw32-gcc -std=c11 -O2 -Icui_src\CUI-master\include -Icui_src\CUI-master\src ^
  -o shot.exe tools\shot.c src\poe_core.c ^
  "cui_src\CUI-master\src\*.c" "cui_src\CUI-master\src\render\*.c" -lgdi32 -luser32
shot.exe ui.png
```

## 常见问题

**链接时报 `undefined reference to 'cui_app_create'`**

漏了 `src\platform\cui_app.c`（以及 Windows 后端 `cui_app_win32.c`）。
这些不在 `src\*.c` 通配符里，必须单独列出。

**运行后立即退出，退出码 `0xC0000135`**

缺少运行库 DLL。确认用了 `-static -static-libgcc`。

**界面中文显示为空白**

程序会自动加载系统 CJK 字体作为 fallback。如果仍然空白，
说明系统里没有常见中文字体，需要在 `poe_patch_gui.c` 的
`setup_cjk_font()` 里补充字体文件名。

**按钮/下拉框文字不显示**

`cui_select` 必须直接放在 `cui_column` 顶层，用 `.label =` 选项，
不要拆成 `cui_label` + `cui_select`，也不要包在 `cui_row` 里。
