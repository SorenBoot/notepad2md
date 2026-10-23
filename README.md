# notepad2md

> 基于 Notepad2 二次开发的 Markdown 编辑器 —— 单文件 exe、实时预览、零运行时依赖。

**项目主页**：<https://github.com/SorenBoot/notepad2md>
**问题反馈**：<https://github.com/SorenBoot/notepad2md/issues>

![界面截图](screenshot.png)

---

## 这是什么

notepad2md 是 [Notepad2 4.2.25](https://www.flos-freeware.ch/) 的二次开发版本。

Notepad2 是 Florian Balmer 于 2004–2011 年开发的轻量级记事本替代软件，以极小的体积、
完整的语法高亮和快捷操作著称。notepad2md 在**完整保留原版全部编辑能力**的前提下，
加入了 Markdown 实时预览与简体中文界面：

- 基于 Notepad2 4.2.25 + Scintilla 2.24 + md4c
- 新增 Markdown 实时预览窗格（MSHTML 引擎，系统自带，无需安装运行时）
- 新增编码自动识别（无 BOM 的 UTF-8 / GBK / Big5 等均可正确打开，原版会乱码）
- 新增简体中文界面（与英文界面打包在同一 exe 内，按系统区域自动切换）
- 其他改动均为使其可用 MinGW-w64 编译的最小必要修改，未改动原版功能逻辑

> 本项目遵循 Notepad2 的 BSD 3-Clause 许可证。
> 原始代码版权归 Florian Balmer (2004–2011)，本项目新增部分的版权归 SorenBoot (2025–2026)。
> 详见 [LICENSE](LICENSE) 与 [LICENSE-THIRD-PARTY/](LICENSE-THIRD-PARTY/)。

## 特性

**体积与依赖**

- **单 exe，约 2.9 MB（x64）** —— 静态链接 libgcc / libstdc++，拷贝即用
- **零运行时依赖** —— 只依赖 Windows 系统 DLL（Win7 起全部自带）
- 内存占用：基线约 15.6 MB（RSS，未开预览时实测）；开启预览后 MSHTML 引擎会额外占用一定内存

**Markdown 预览**

- **实时预览** —— 左右分栏，边打边看（`Ctrl+Alt+M` 开关）
- **可拖动分隔条** —— 拖动调整两栏宽度，双击复位 50/50，比例自动记忆
- **刷新不闪屏** —— 输入连续时合并刷新，不会每敲一个字都重排
- **GFM 语法** —— 表格、任务列表、删除线、脚注、围栏代码块
- **代码块复制按钮** —— 预览里的围栏代码块带语言标签与「复制」按钮
- **链接外开** —— 预览里的链接用系统浏览器打开，不在窗格内跳转
- **安全** —— HTML 标签被转义，预览不执行脚本

**格式整理（JSON / XML / SQL / INI / TOML）**

- 顶层「格式」菜单三项：**格式化**（`Ctrl+Alt+F`）、**压缩**（`Ctrl+Alt+Shift+F`）、**校验**（`Ctrl+Alt+V`）
- 支持类型：**JSON / JSONC / JSON5 / JSON Lines**、**XML / HTML**、**SQL**、**INI / TOML / .env**、**纯文本 / .log**
  —— 按当前文件的语法方案自动选择；拿不准时按内容嗅探- **只动空白，不动字节** —— 不解析成对象树再重新序列化，所以键顺序、数字字面形式
  （`1.0` / `1e3` / `0x1F`）、大整数精度、重复键、Unicode 转义、注释、XML 属性顺序
  与实体写法全部原样保留
- **两条可验证的硬不变量**：`压缩(格式化(x))` 与 `压缩(x)` 逐字节相同；
  `格式化(格式化(x))` 与 `格式化(x)` 逐字节相同（`tests/run/test_format.c` 全文校验）
- **有选区只处理选区** —— 选中一段 JSON 片段单独整理，其余内容不受影响
- **行尾沿用文件** —— LF 文件还是 LF，CRLF 文件还是 CRLF；缩进宽度跟随当前的 Tab 设置
- **一次撤销** —— 整个整理动作是一个撤销步，`Ctrl+Z` 一步回到原样
- **出错不改文件** —— 语法不合法时原文一个字节都不动，弹窗给出「第几行第几列」并把光标移过去
- **大文件行为** —— 超过 8 MB 时弹窗确认，16 MB 以上直接拒绝（引擎上限也是 16 MB）
- **保守的地方是故意的** —— XML 混合内容（`<p>前 <b>粗</b> 后</p>`）、`<pre>` / `<script>` /
  `xml:space="preserve"`、TOML 多行数组与多行字符串一律原样保留；标签不配对直接报错，
  绝不猜隐式闭合；YAML 缩进即语义，不提供格式化
- **日志语法高亮** —— `.log` / `.logfile` 文件以及无扩展名的 `messages` / `syslog` / `dmesg` 等
  自动按「日志」方案着色：
  日期时间（`2024-01-01 10:00:00` / `T` / 毫秒 / 时区）、四档级别
  （FATAL / ERROR / WARN / INFO，含 `[INFO]`、`inf`、`[INF]` 等缩写与包裹写法）、
  数字与 IP:端口、引号字符串、`key=value` 的键、`//` 与 `#` 注释、
  `(Foo.java:42)` 这类位置；堆栈缩进原样不动
- **.log / 纯文本只整理空白** —— 去掉每行行尾空白、连续空行压成一个、文件开头不留空行；
  行首缩进原样保留（堆栈靠缩进分层），也绝不按时间戳/级别重排字段

**编码处理**

- **打开时自动识别编码** —— 无需手动选择，无 BOM 的 UTF-8、GBK、Big5、Shift-JIS、EUC-KR 均可正确识别
- **识别准确** —— 对候选编码做严格校验，不会把 GBK 误判成 Big5
- **只读文件头 64 KB** 做判断，大文件不会卡顿
- **猜错可手动纠正** —— 以 ANSI / OEM / UTF-8 / Unicode 等方式重新打开（`Ctrl+Shift+A` / `Ctrl+Shift+O` / `Ctrl+Shift+F` / `Shift+F8`）
- **重新编码** —— `F8` 把当前内容转成另一种编码保存；`F9` 选择编码

> 打开无 BOM 的 UTF-8 文件时，原版 Notepad2 会按系统代码页误读成乱码；本项目修复了这一问题。

**编辑能力（继承自 Notepad2）**

- Scintilla 语法高亮（完整保留原版全部快捷键）
- 代码折叠、列选择、正则查找替换、编码转换
- 窗口置顶（`Alt+T`）

**同词高亮（不限格式）**

- 双击选中一个词，全文相同的词立即高亮（`Ctrl+Alt+W` 开关，默认关）
- 不依赖任何语法方案 —— 纯文本、日志、代码一视同仁
- 只高亮**完整独立的词**：`alphabet` 里的 `alpha` 不会被误涂
- 选区不是完整单词、或多选/矩形选择时自动清除，不多涂
- 取消选区即清除；可随设置记忆

**界面语言**

- 简体中文与英文两套界面打包在同一 exe 内：
  - 系统区域为中文（中国大陆）→ 显示中文界面
  - 其他区域 → 显示英文界面
- 想在中文系统上强制用英文界面，可用 [Resource Hacker](http://www.angusj.com/resourcehacker/)
  调整资源语言，或把 `src/Notepad2.rc` 里中文块结尾的 `#endif` 注释掉后重新编译

## 系统要求

| 项目 | 要求 |
|---|---|
| 操作系统 | Windows 7 SP1 / 8 / 8.1 / 10 / 11 |
| 架构 | x86 与 x64 均可 |
| 运行时 | **无**（静态链接，无需 VC++ 运行库等） |

当前仓库提供的是 **x64** 预编译版本；32 位版本可自行编译，源码对两种架构均无限制。

预览使用系统自带的 MSHTML 引擎（非 WebView2），因此无需额外安装运行时，代价是 CSS
支持止步于 IE11 水平（不支持 CSS 变量 / Grid / `flex gap`）。

## 获取与编译

### 方式一：下载编译好的版本

前往 [Releases](https://github.com/SorenBoot/notepad2md/releases) 页面下载最新版本。

### 方式二：自行编译

需要 **MinGW-w64**。推荐便携版 [w64devkit](https://github.com/skeeto/w64devkit)
（约 100 MB，解压即用）。

```
build\build.bat          双击即可编译，无需 make
```

或：

```bash
cd build
make                    # Release x64
make DEBUG=1            # Debug
make clean
make run
```

产物输出到 `bin\notepad2md.exe`。

构建脚本默认产出 **x64**。要编 32 位，把 w64devkit 换成 i686 工具链后执行 `make` 即可，
源码未对架构作任何限定。

默认工具链路径为 `C:\Tools\W64DEVKIT\w64devkit\w64devkit`，装在别处时：

```bash
# build.bat 里改
set W64DEVKIT=你的路径

# 或 make 时传参
make W64DEVKIT=/d/your/w64devkit
```

## 目录结构

```
notepad2md/
├── build/                构建脚本（Makefile + build.bat）
├── src/                  源码
│   ├── Notepad2.c        主程序
│   ├── Edit.c            编辑区逻辑
│   ├── Dialogs.c         对话框
│   ├── Styles.c          配色方案
│   ├── Dlapi.c           目录/文件列表 API
│   ├── Helpers.c         工具函数
│   ├── Print.cpp         打印
│   ├── Markdown.c        Markdown 解析（新增）
│   ├── Preview.c         预览渲染（新增）
│   ├── Format.c          格式化引擎入口 / 输出器（新增）
│   ├── Format_Json.c     JSON / JSONC / JSON5 重排（新增）
│   ├── Format_Xml.c      XML / HTML 重排（新增）
│   ├── Format_Sql.c      SQL 重排（新增）
│   ├── Format_Ini.c      INI / TOML / .env 重排（新增）
│   ├── Format_Log.c      纯文本 / .log 重排（新增）
│   └── FormatCmd.c       格式化功能的编辑器接线（新增）
├── scintilla/            编辑引擎（仅编译必需文件）
├── md4c/                 Markdown 解析器（MIT）
├── res/                  图标 / 光标 / 位图 / manifest
├── screenshot.png        界面截图（README 引用）
└── LICENSE-THIRD-PARTY/  第三方许可证
```

> `docs/` 与 `tests/` 在本地开发时使用，未纳入公开仓库。

## 许可证

本项目沿用上游 Notepad2 的 **BSD 3-Clause** 许可证（见 [LICENSE](LICENSE)）：

- 原始 Notepad2 代码：Copyright (c) 2004-2011 **Florian Balmer**
- 本项目新增代码：Copyright (c) 2025-2026 **SorenBoot**

第三方组件许可证见 [LICENSE-THIRD-PARTY/](LICENSE-THIRD-PARTY/)：

- Notepad2 —— BSD 3-Clause
- Scintilla —— 上游自带许可证（BSD 风格，见 [License](LICENSE-THIRD-PARTY/Scintilla-License.txt)）
- md4c —— MIT（[License](LICENSE-THIRD-PARTY/md4c-License.md)）

## 致谢

- [Florian Balmer](https://www.flos-freeware.ch/) —— Notepad2 原作者
- [Neil Hodgson](https://www.scintilla.org/) —— Scintilla
- [Martin Mitas](https://github.com/mity) —— md4c

---

*notepad2md 是非官方的二次开发项目，与 Notepad2 原作者无关联。*
