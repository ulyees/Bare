# Bare

极简 Windows 文本编辑器 —— 只有「打开 → 编辑 → 保存」，以及一个查找框。

- **单文件免安装**：一个 231 KB 的 `Bare.exe`，双击即开，不依赖 .NET / Python / Electron / WebView。
- **打开极快**：80 字节小文件冷启动到可用约 **85 ms**；64 MB / 70 万行文本约 **0.25 s** 打开。
- **大文件友好**：内存映射读取（`CreateFileMapping`）+ 只绘制屏幕上可见的那几十行，滚动不卡。
- **页面现代**：自绘扁平工具栏、淡行号、当前行高亮、跟随系统浅色/深色主题、Segoe UI + Consolas、ClearType、高 DPI、Windows 11 风格深色标题栏。
- **不乱改文件**：自动识别 UTF-8 / UTF-8 BOM / UTF-16 LE / UTF-16 BE（含无 BOM）/ ANSI-GBK，保存时按原编码、原换行风格、原文件末尾换行状态写回。

![Bare 界面](docs/screenshot.png)

## 下载 / 使用

直接下载仓库里的 [`Bare.exe`](Bare.exe) 双击即可，绿色免安装，删除即卸载。

| 打开方式 | 操作 |
| --- | --- |
| 空白文档 | 双击 `Bare.exe`，直接输入，Ctrl+S 保存 |
| 打开已有文件 | 点右上角「打开」，或 Ctrl+O，或把文件拖进窗口 |
| 命令行 | `Bare.exe D:\notes\a.txt` |
| 设为默认程序 | 右键 .txt → 打开方式 → 选择其他应用 → 更多应用 → 在这台电脑上查找 → 选中 `Bare.exe` → 勾选「始终使用此应用打开 .txt 文件」 |

## 快捷键

| 按键 | 作用 |
| --- | --- |
| Ctrl+N | 新建空白文档 |
| Ctrl+O | 打开文件 |
| Ctrl+S | 保存（未命名文档会弹出另存为） |
| Ctrl+Shift+S | 另存为 |
| Ctrl+F | 打开/聚焦查找框 |
| F3 / Shift+F3 | 查找下一个 / 上一个 |
| Enter / Shift+Enter（查找框内） | 下一个 / 上一个 |
| Esc | 关闭查找框 / 取消选择 |
| Ctrl+A / Ctrl+C / Ctrl+X / Ctrl+V | 全选 / 复制 / 剪切 / 粘贴 |
| Ctrl+Home / Ctrl+End | 跳到文档首 / 尾 |
| 方向键、Home、End、PgUp、PgDn | 移动光标（配合 Shift 选择文本） |
| 滚轮 / Shift+滚轮 | 垂直滚动 / 横向滚动 |
| Tab | 插入 4 个空格 |

## 查找

按 **Ctrl+F** 打开窗口内查找条，直接输入即时高亮全部匹配并显示 `当前/总数`：

- `▲` / `▼` 或 Enter / Shift+Enter 跳转匹配，F3 / Shift+F3 在窗口内也能用。
- `Aa` 按钮切换区分大小写。
- `×` 或 Esc 关闭查找条。

## 编码与换行

| 检测 | 处理 |
| --- | --- |
| BOM | UTF-8 / UTF-16 LE / UTF-16 BE 直接识别 |
| 无 BOM UTF-16 | 按零字节分布启发式识别 |
| 合法 UTF-8 | 按 UTF-8 打开 |
| 合法 ANSI | 按系统代码页（简体中文为 GBK）打开 |
| 其它（含 NUL / 非法字节） | 进入**字节模式**，每字节一字符、控制字符显示为 `·`，保存时逐字节还原 |

保存时保留原编码与 BOM，换行风格（CRLF / LF）与文件末尾是否有换行也原样保留。

## 为什么快

- 直接 Win32 API + GDI 自绘界面，没有框架初始化、没有 WebView、没有 UI 控件树。
- 文件用内存映射读取，编码识别只扫描 BOM 和少量启发式字节，行切分一次性完成并预留容量。
- 绘制只处理可见行：`ExtTextOutW` 逐行输出，行号、选中、当前行、查找高亮都在同一趟双缓冲里画完，无闪烁。

实测（本机 Windows 10 x64，冷启动到窗口可交互）：

| 文件 | 大小 | 打开耗时 | 内存 |
| --- | --- | --- | --- |
| 小文本 | 80 B | 85 ms | 22 MB |
| 大文本 | 64.4 MB / 70 万行 | 255 ms | 182 MB |

## 自己编译

源码是单个 `Bare.cpp`（Win32 + C++17，无第三方依赖）。

MSVC（推荐，静态 CRT，生成的 exe 免运行库）：

```bat
build-msvc.bat
```

脚本会自动定位 Visual Studio 的 `vcvars64.bat`；也可以手动在 x64 Native Tools Command Prompt 中执行：

```bat
rc /nologo /fo app_res.res app.rc
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 Bare.cpp app_res.res /link /SUBSYSTEM:WINDOWS /OUT:Bare.exe user32.lib gdi32.lib comdlg32.lib dwmapi.lib shell32.lib advapi32.lib
```

MinGW-w64（需要 `g++` 与 `windres`）：

```bat
build.bat
```

注意：用 MSVC 编译时源码里必须有 `#define NOMINMAX`（已包含），否则 `std::max/std::min` 会被 `windows.h` 的宏破坏。

## 目录结构

```
Bare.cpp       全部源码（界面、编辑、编码、查找）
app.rc         资源脚本（图标 + 清单）
app.ico        图标
app.manifest   DPI / 主题 / 通用控件 6 清单
make_icon.py   重新生成 app.ico（可选）
build-msvc.bat MSVC 构建
build.bat      MinGW-w64 构建
```

## 刻意没有的东西

标签页、撤销/重做、语法高亮、代码折叠、设置面板、插件、自动保存、文件树、统计面板。每一项都会带来分支逻辑和体积，这里一个都不留。

> 因为没有撤销，改动重要文件前建议先备份。

## 系统要求与限制

- Windows 7 x64 及以上；沉浸式深色标题栏需要 Win10 1903+（不影响功能）。
- 超大文本内存占用约为文件大小的 2～3 倍；超过 512 MB 会先询问，超过 2 GB 会跳过。
- 超长单行使用横向滚动，不做自动折行，以保持最短渲染路径。
- 二进制文件可以查看，但不适合当二进制编辑器用。

## 许可

[MIT](LICENSE) © 2026 ulyees
