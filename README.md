# POE_patch_CHT

**Path of Exile 繁体中文语系 + 自定义字体 修改工具**

> ⚠️ **第一版仅针对 Steam 版本的 Path of Exile。**
>
> 本工具读取的是 Steam 客户端使用的 `Bundles2` 目录结构
> （`_.index.bin` + `*.bundle.bin`）。
> 官方独立客户端（Content.ggpk）和 Epic 版本**尚未支持**，
> 后续版本会考虑加入。

[简体中文](#简体中文) | [English](#english)

---

## 简体中文

### 这是什么

一个用 **[CUI](https://github.com/yappologistic/CUI)**（纯 C 的即时模式 UI 库）写的原生 C 图形工具，
用于在 Path of Exile 游戏更新后，一键重新应用两项改动：

1. **繁体中文语系** —— 让游戏内的「法语（Français）」入口显示繁体中文
2. **自定义字体** —— 把繁体中文 UI 的字体改成你指定的系统字体

因为游戏每次更新都会覆盖 `_.index.bin`，所以原来的修改会失效。
更新后重新运行一次本工具即可。

### 原理

PoE 的语言数据存放在 `Data/Languages.dat`，共 11 种语言，每行有三个关键字段：

| 字段 | 例（法语行） | 例（繁中行） |
|---|---|---|
| name | `French` | `Traditional Chinese` |
| native | `Français` | `繁體中文` |
| code | `fr` | `zh-TW` |

游戏读取 `production_Config.ini` 里的 `language=fr`，找到 `code == "fr"` 的那一行，
再用该行的 **name** 去加载 `data/<name 小写>/` 目录下的文本数据。

本工具做的事情是：**交换 `fr` 行与 `zh-TW` 行的 name 和 native，code 保持不变**。

交换后：

- `code=fr` 的行 → `name=Traditional Chinese`，`native=繁體中文`
  游戏选法语时，实际加载 `data/traditional chinese/` 的繁中数据，界面显示「繁體中文」
- `code=zh-TW` 的行 → `name=French`，`native=Français`（占位）

这个行为与官方工具 [PoeChinese3](https://github.com/aianlinb/LibGGPK3/tree/main/Examples/PoeChinese3)
的结果**逐字节一致**（已用 v2.7.5 版本做过比对验证）。

字体的修改则是把 `Metadata/UI/UISettings.Traditional Chinese.xml`
里所有 `typeface="..."` 替换成你选定的字体名。

### 界面

```
┌─────────────────────────────────────────────────┐
│ Path of Exile 繁中语系 + 字体                    │
│                                                 │
│ 游戏索引文件                                     │
│ ┌─────────────────────────────────────────────┐ │
│ │ D:\Games\Steam\...\Bundles2\_.index.bin     │ │
│ │ [选择文件...] [重新检查]                     │ │
│ └─────────────────────────────────────────────┘ │
│                                                 │
│ 字体                                             │
│ [ HarmonyOS Sans TC          ▼ ]                │
│                                                 │
│ (语言已应用) (字体已应用)                        │
│ fr    -> Traditional Chinese  (繁體中文)         │
│ zh-TW -> French  (Français)                     │
│                                                 │
│ [修改字体] [修改语言到繁体中文] [应用全部修改]    │
└─────────────────────────────────────────────────┘
```

- 顶部自动探测常见的 Steam 安装路径；找不到时点「选择文件...」手动定位 `_.index.bin`
- 字体下拉菜单只列出**本机实际安装的**中文字体（通过 Win32 `CreateFont` + `GetTextFace` 检测）
- 三项操作都是**幂等**的：已应用时会自动跳过，重复点击不会重复写入

### 使用方法

1. 从 [Releases](../../releases) 下载 `POE_patch_CHT_v*_win-x64.zip`
   （**压缩包内已包含 `oo2core.dll`**，解压即用）
2. **先关闭游戏**
3. 双击 `poe_patch.exe`
4. 确认索引路径正确，选择想要的字体
5. 点「应用全部修改」
6. 进游戏，语言选「法语」

也可以命令行指定路径：

```bat
poe_patch.exe "D:\Games\Steam\steamapps\common\Path of Exile\Bundles2\_.index.bin"
```

### 关于 oo2core.dll

游戏的 bundle 文件用 **Oodle** 压缩。`oo2core.dll` 是 Oodle 的运行时库。

#### 来源

本仓库的 **Release 压缩包里附带**了 `oo2core.dll`，方便直接使用。
该文件取自 [LibGGPK3](https://github.com/aianlinb/LibGGPK3/releases) 官方发布的
`win-x64.zip`，未做任何修改。

#### 授权声明

**`oo2core.dll` 属于 Epic Games 的专有软件（Oodle Data Compression），
不是本项目的组成部分，也不适用本项目的 MIT 许可证。**

- 它仅随本工具用于读取**用户本机的游戏文件**，本工具不修改、不打包、不再分发该库
- 源码仓库（`src/`）**不包含**此文件，仓库内容仍为纯 MIT
- 该文件仅出现在 Release 附件中，不单独提供下载
- 如需在其他项目中使用 Oodle，请自行向 **Epic Games** 获取合法授权
  （[RAD Game Tools / Oodle](https://www.radgametools.com/oodle.htm)）

#### 手动获取

如果你不希望使用附件中的版本，也可以自行获取，效果相同：

- 下载 [LibGGPK3 官方 Release](https://github.com/aianlinb/LibGGPK3/releases)
- 解压 `win-x64.zip`
- 取出里面的 `oo2core.dll`，放到 `poe_patch.exe` 旁边

### 从源码构建

需要：

- **MinGW-w64**（gcc + windres）
- **Python 3.8+**（仅用于生成图标，可选）
- **CUI** 源码（`cui.h` / `cui.c` 等）

```
git clone https://github.com/yappologistic/CUI.git cui_src/CUI-master
build.bat
```

`build.bat` 会自动：生成图标 → 编译资源 → 静态链接成 exe。

> 说明：本项目用 `-static` 静态链接，生成的 exe 不依赖 `libwinpthread-1.dll`
> 等 MinGW 运行时，可直接分发。

### 文件结构

```
POE_patch_CHT/
├── src/
│   ├── poe_core.h       核心接口
│   ├── poe_core.c       Oodle 封装、bundle 读写、索引解析、Languages.dat / XML 处理
│   ├── poe_patch_gui.c  CUI 界面 + 中文字体回退
│   └── app.rc           图标与版本信息资源
├── tools/
│   ├── make_icon.py     生成 app.ico（纯 Python，无需 Pillow）
│   ├── test_core.c      核心逻辑测试（只读，安全）
│   ├── test_e2e.c       端到端测试（加 write 参数才写回）
│   ├── shot.c           离屏渲染界面截图（无需窗口）
│   ├── check_png.py     验证截图像素
│   ├── check_res.py     验证 exe 内嵌资源
│   └── preview_icon.py  图标 ASCII 预览
├── app.ico              应用图标（7 种尺寸）
└── build.bat            一键构建
```

### 技术要点

- **Oodle 解压/压缩**：通过 `oo2core.dll` 的 `OodleLZ_Decompress` / `OodleLZ_Compress`
- **不原地修改**：新数据写入新建的 `LibGGPK3/0.bundle.bin`，
  再重写 `_.index.bin` 让目标文件指向它
  —— 与 LibGGPK3 官方策略一致，避免破坏原 bundle
- **写前自校验**：先把生成的数据写成临时文件并解压比对，
  确认 bundle 大小、索引结构、文件记录、目录表都正确后，才替换正式文件
- **原子写入**：写 `.tmp` 后 `rename`
- **中文字体回退**：CUI 内置字体（Inter / Geist）不含 CJK 字形，
  程序启动时自动加载系统中文字体（优先微软雅黑 `msyh.ttc`）作为 fallback，
  否则所有中文会显示为空白

### 已知限制

- **仅支持 Steam 版**。独立客户端（`Content.ggpk`）和 Epic 版不支持
- **游戏更新后需重新运行**。Steam 校验完整性也可能还原索引，届时重跑即可
- **要求游戏关闭**。文件被占用时写入会失败
- **字体需系统可识别**。若游戏以管理员权限运行但字体系装在用户级目录，
  可能读不到，需把字体装到全局

### 验证情况

在隔离的 testbed 环境（复制真实 bundle 副本）中验证过：

- 索引完整性：46885 bundles / 1245702 files / 86209 dirs 全部正确
- `languages.dat` 输出与官方 PoeChinese3 v2.7.5 **逐字节一致**
- 语言交换正确：`fr` → `Traditional Chinese` / `繁體中文`
- 字体正确写入，UTF-16 LE + BOM 保持不变
- 二次应用可精确还原原始文件（幂等）

### 免责声明

本工具仅修改你本机的游戏数据文件，**不修改任何线上数据**。
使用风险自负。修改前建议备份 `Bundles2\_.index.bin`。

本项目与 Grinding Gear Games 无关。

---

## English

**Path of Exile Traditional Chinese language + custom font patcher**

> ⚠️ **Version 1 supports the Steam version of Path of Exile only.**
> It works with the `Bundles2` directory layout used by Steam
> (`_.index.bin` + `*.bundle.bin`). The standalone client (`Content.ggpk`)
> and Epic versions are **not yet supported**.

### What it does

A native C GUI built with **[CUI](https://github.com/yappologistic/CUI)** that
re-applies two modifications after a Path of Exile game update:

1. **Traditional Chinese locale** — makes the in-game "Français" option display
   Traditional Chinese
2. **Custom font** — changes the Traditional Chinese UI font to a system font
   of your choice

### How it works

The game reads `language=fr` from `production_Config.ini`, finds the
`Data/Languages.dat` row whose `code` is `"fr"`, then loads `data/<lowercase name>/`
using that row's **name** field.

The patcher **swaps the `name` and `native` fields** of the `fr` and `zh-TW` rows,
leaving `code` untouched. After that, choosing French loads the
`data/traditional chinese/` data and the menu shows 「繁體中文」.

This matches the output of the official
[PoeChinese3](https://github.com/aianlinb/LibGGPK3/tree/main/Examples/PoeChinese3)
tool byte-for-byte (verified against v2.7.5).

### Requirements

- Windows 7+
- Steam version of Path of Exile

### Usage

1. Download `POE_patch_CHT_v*_win-x64.zip` from [Releases](../../releases)
   (the archive **already contains `oo2core.dll`** — just unzip and run)
2. Close the game
3. Run `poe_patch.exe`
4. Verify the index path, pick a font, click the apply button
5. In game, select "Français" as the language

### Building

Requires MinGW-w64 (gcc + windres) and optionally Python for icon generation.

```
git clone https://github.com/yappologistic/CUI.git cui_src/CUI-master
build.bat
```

### About oo2core.dll

`oo2core.dll` is the runtime for **Oodle Data Compression**.

- **Source**: the copy bundled in the Release archive is taken unmodified from
  the official [LibGGPK3](https://github.com/aianlinb/LibGGPK3/releases) `win-x64.zip`.
- **Licensing**: `oo2core.dll` is proprietary software of **Epic Games**
  (Oodle Data Compression). It is **not part of this project** and is **not
  covered by this project's MIT license**.
- It is used only to read **your local game files**. This project does not
  modify or redistribute it. The source repository (`src/`) does **not**
  contain the file — the repo remains pure MIT; the library appears only as a
  Release attachment.
- For any other use of Oodle, obtain a proper license from
  [Epic / RAD Game Tools](https://www.radgametools.com/oodle.htm).
- Prefer to get it yourself? Take `oo2core.dll` from the LibGGPK3 release and
  place it next to `poe_patch.exe`.

### License

Source code: MIT.
Oodle (`oo2core.dll`) is proprietary (Epic Games) and is not part of the
repository; it is included only in the Release archive.
UI: [CUI](https://github.com/yappologistic/CUI) (MIT).
Icons: Lucide (ISC), bundled with CUI.

Not affiliated with Grinding Gear Games.
