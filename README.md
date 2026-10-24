# notepad2md

> 基于 Notepad2 二次开发的 Markdown 编辑器 —— 单文件 exe、实时预览、零运行时依赖。

**项目主页**：<https://github.com/SorenBoot/notepad2md>
**问题反馈**：<https://github.com/SorenBoot/notepad2md/issues>

![界面截图](screenshot.png)

---

## 这是什么

notepad2md 是 [Notepad2 4.2.25](https://www.flos-freeware.ch/) 的二次开发版本，在完整保留
原版全部编辑能力的前提下，加入了：

- Markdown 实时预览
- 无 BOM 编码自动识别
- JSON / XML / SQL / INI / TOML 格式化与校验
- 日志语法高亮
- 双击选词、全文同词高亮
- 一键运行当前文件（带输出面板）
- 简体中文界面

> 本项目遵循 Notepad2 的 BSD 3-Clause 许可证。
> 原始代码版权归 Florian Balmer (2004–2011)，本项目新增部分的版权归 SorenBoot (2025–2026)。
> 详见 [LICENSE](LICENSE) 与 [LICENSE-THIRD-PARTY/](LICENSE-THIRD-PARTY/)。

## 特性

### Markdown 预览

- **实时预览** —— 左右分栏，边打边看（`Ctrl+Alt+M` 开关）
- 可拖动分隔条调整两栏宽度，双击复位，比例自动记忆
- 输入连续时合并刷新，不闪屏
- 支持 GFM：表格、任务列表、删除线、脚注、围栏代码块
- 代码块带语言标签与「复制」按钮
- 链接用系统浏览器打开，HTML 标签被转义，不执行脚本

### 格式化 / 压缩 / 校验

顶层「格式」菜单三项：**格式化**（`Ctrl+Alt+F`）、**压缩**（`Ctrl+Alt+Shift+F`）、**校验**（`Ctrl+Alt+V`）。

- 支持：**JSON / JSONC / JSON5 / JSON Lines**、**XML / HTML**、**SQL**、**INI / TOML / .env**
- 按当前文件的语法方案自动选择；拿不准时按内容嗅探
- 只重排空白，不改任何内容
- 有选区只处理选区
- 行尾沿用文件原有风格；缩进跟随当前 Tab 设置
- 出错时原文一个字节都不动，弹窗给出「第几行第几列」
- 格式化超大文件会提示，**不影响打开和编辑**

### 日志语法高亮

`.log` / `.logfile`，以及无扩展名的 `messages` / `syslog` / `dmesg` 等日志文件，自动着色：

- 日期时间（`2024-01-01 10:00:00` / ISO / 毫秒 / 时区 / 月份缩写）
- 四档级别配色：FATAL（红底）、ERROR（红）、WARN（橙）、INFO（绿）、DEBUG/TRACE（灰）
  —— 含 `INFO` / `inf` / `[INFO]` / `[INF]` 等缩写与包裹写法
- 数字与 IP:端口、引号字符串、`key=value` 的键、`//` 与 `#` 注释、`(Foo.java:42)` 位置
- 堆栈缩进原样不动

### 同词高亮

- 双击选中一个词，全文相同的词立即高亮（`Ctrl+Alt+W` 开关，默认开）
- 不依赖语法方案 —— 纯文本、日志、代码一视同仁
- 只高亮完整独立的词（`alphabet` 里的 `alpha` 不涂）

### 运行当前文件

- `F5` 直接跑当前文件，输出进下方输出面板（不弹黑窗）
- 按扩展名选解释器：`.py` / `.js` / `.bat` / `.cmd` / `.ps1` / `.sh` 等
- 无扩展名时按 shebang 识别
- 输出面板：彩色输出、错误行点击跳转、可停靠/浮动/缩放
- 面板变量：`%FILE%` `%DIR%` `%NAME%` `%EXT%` `%LINE%` `%SEL%`，命令历史持久化

### Markdown 大纲

侧栏列出全部标题，点击跳转，随编辑实时更新。

### 编码处理

- 打开时自动识别编码 —— 无 BOM 的 UTF-8 / GBK / Big5 / Shift-JIS / EUC-KR 均可正确识别
- 大文件不会卡顿
- 猜错可手动重新打开（`Ctrl+Shift+A` / `Ctrl+Shift+O` / `Ctrl+Shift+F` / `Shift+F8`）
- `F8` 转换编码保存，`F9` 选择编码

### 编辑能力（继承自 Notepad2）

- Scintilla 语法高亮（完整保留原版全部快捷键）
- 代码折叠、列选择、正则查找替换
- 窗口置顶（`Alt+T`）

### 界面语言

简体中文与英文两套界面打包在同一 exe 内，按系统区域自动切换。

### 其它

- Explorer 右键菜单「用 notepad2md 编辑」（只写 HKCU，可注销）
- 零运行时依赖 —— 只依赖 Windows 系统 DLL（Win7 起全部自带）

## 系统要求

| 项目 | 要求 |
|---|---|
| 操作系统 | Windows 7 SP1 / 8 / 8.1 / 10 / 11 |
| 架构 | x86 与 x64 均可 |
| 运行时 | **无** |

各版本改动见 [CHANGELOG.md](CHANGELOG.md)。

## 获取与编译

### 方式一：下载编译好的版本

前往 [Releases](https://github.com/SorenBoot/notepad2md/releases) 页面下载最新版本。

### 方式二：自行编译

需要 **MinGW-w64**。推荐便携版 [w64devkit](https://github.com/skeeto/w64devkit)（解压即用）。

```
build\build.bat          双击即可编译，无需 make
```

或：

```bash
cd build
make                    # Release x64
make clean
```

产物输出到 `bin\notepad2md.exe`。工具链路径等细节见 `build\build.bat` 顶部的注释。

## 许可证

本项目沿用上游 Notepad2 的 **BSD 3-Clause** 许可证（见 [LICENSE](LICENSE)）：

- 原始 Notepad2 代码：Copyright (c) 2004-2011 **Florian Balmer**
- 本项目新增代码：Copyright (c) 2025-2026 **SorenBoot**

第三方组件许可证见 [LICENSE-THIRD-PARTY/](LICENSE-THIRD-PARTY/)。

## 致谢

- [Florian Balmer](https://www.flos-freeware.ch/) —— Notepad2 原作者
- [Neil Hodgson](https://www.scintilla.org/) —— Scintilla
- [Martin Mitas](https://github.com/mity) —— md4c

---

*notepad2md 是非官方的二次开发项目，与 Notepad2 原作者无关联。*
